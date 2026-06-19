#include "api.h"
#include "schema_catalog.h"
#include "query_builder.h"
#include "row_json.h"
#include "core/db_connection.h"
#include "core/auth.h"
#include "core/mfa.h"
#include "core/oauth.h"
#include "core/mailer.h"
#include "core/base64url.h"
#include "core/metrics.h"
#include "logger.h"

#include <libpq-fe.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SESSION_TTL_SECONDS (24 * 3600)
#define MIN_PASSWORD_LEN 8
/* Cap the password length: Argon2id processes the whole input, so a multi-MB
 * password is a CPU-DoS. 128 is well above any real password. */
#define MAX_PASSWORD_LEN 128

static pgf_api_result_t session_result(const char *token, const pgf_user_t *user);
static void send_email_verification(const char *user_id);

static pgf_api_result_t result_error(int status, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "message", message);
    pgf_api_result_t r = { o, status };
    return r;
}

static const pgf_table_t *resolve_table(const cJSON *req) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "table");
    if (!cJSON_IsString(t)) return NULL;
    return pgf_catalog_find(pgf_catalog_active(), t->valuestring);
}

/* Map a Postgres SQLSTATE to an HTTP status (client vs server error). */
static int map_sqlstate(const char *ss) {
    if (!ss) return 500;
    if (!strcmp(ss, "23505")) return 409;   /* unique_violation */
    if (!strcmp(ss, "23503")) return 400;   /* foreign_key_violation: bad reference */
    if (!strcmp(ss, "23502")) return 400;   /* not_null_violation */
    if (!strcmp(ss, "23514")) return 400;   /* check_violation */
    if (ss[0] == '2' && ss[1] == '2') return 400;  /* data exceptions (bad uuid, range, ...) */
    return 500;
}

/* Generic, client-safe message for a client-error SQLSTATE. NEVER return the raw
 * libpq primary message: it embeds constraint / index / table / column names and
 * the offending values (schema disclosure), and a unique-violation message is a
 * cross-tenant row-existence oracle since UNIQUE is enforced BELOW row-level
 * security. The full message is still logged server-side. (M-7) */
static const char *sqlstate_message(const char *ss) {
    if (!ss) return "request failed";
    if (!strcmp(ss, "23505")) return "conflict";
    if (!strcmp(ss, "23503")) return "invalid reference";
    if (!strcmp(ss, "23502")) return "a required field is missing";
    if (!strcmp(ss, "23514")) return "a value failed a constraint check";
    if (ss[0] == '2' && ss[1] == '2') return "invalid input value";
    return "request failed";
}

/* The value to bind to app.tenant_id for Postgres RLS on this request, or NULL
 * when there is no RLS context (single-tenant). "*" is the platform-admin
 * sentinel (sees all tenants); otherwise the caller's tenant. A tenant user with
 * no tenant never reaches here — make_scope denies it first. */
static const char *rls_tenant_setting(const pgf_identity_t *who) {
    if (!pgf_tenancy_column()) return NULL;                  /* single-tenant: no RLS */
    if (!strcmp(who->role, "platform_admin")) return "*";   /* global */
    return who->tenant_id;
}

/* Run a built query, returning its rows as JSON (RETURNING/SELECT). On error,
 * sets *http from SQLSTATE and copies a client-safe message into errmsg.
 *
 * When rls_tenant is non-NULL (pooled mode), the query runs inside a transaction
 * that first binds app.tenant_id via set_config(..., is_local=true). is_local is
 * transaction-scoped, so it shares the transaction with the query and is reset at
 * COMMIT/ROLLBACK — it can never leak to the next request that reuses this pooled
 * connection. This is the DB-level seatbelt behind the app-level scope rules:
 * with the server connected as a non-superuser role, RLS confines every query to
 * the caller's tenant even if the app-level scope were somehow bypassed. */
#ifdef LIBPQ_HAS_PIPELINING
/* Pooled-mode fast path (opt-in, PGF_DB_PIPELINE=1): send BEGIN + set_config
 * (app.tenant_id) + the query + COMMIT as ONE pipelined round trip instead of
 * four. Both inner statements ride the per-connection prepared-statement cache
 * (PQsendQueryPrepared). Returns 1 if it ran the pipeline (out/http/errmsg set),
 * or 0 if it declined before sending anything (connection untouched → caller runs
 * the sequential path). The tenant-scoped transaction semantics are identical to
 * run_rows: set_config(..., is_local=true) binds app.tenant_id for the txn only,
 * and a query error aborts the pipeline so Postgres rolls the txn back. */
static int run_rows_pipelined(PGconn *c, const pgf_query_t *q, const pgf_table_t *t,
                              const char *rls_tenant, cJSON **out,
                              int *http, char *errmsg, size_t errlen) {
    static const char *SET_SQL = "SELECT set_config('app.tenant_id', $1, true)";
    const char *tparams[1] = { rls_tenant };

    /* Prepare both statements (one-time per connection) BEFORE entering pipeline
     * mode, so we can decline cleanly on a miss without a half-built pipeline. */
    char set_name[24], main_name[24];
    if (db_connection_prepare_cached(c, SET_SQL, 1, set_name, sizeof set_name) < 0)
        return 0;
    if (db_connection_prepare_cached(c, q->sql, q->nparams, main_name, sizeof main_name) < 0)
        return 0;
    if (PQenterPipelineMode(c) != 1)
        return 0;

    int queued =
        PQsendQueryParams(c, "BEGIN", 0, NULL, NULL, NULL, NULL, 0) == 1 &&
        PQsendQueryPrepared(c, set_name, 1, tparams, NULL, NULL, 0) == 1 &&
        PQsendQueryPrepared(c, main_name, q->nparams,
                            (const char *const *)q->params, NULL, NULL, 0) == 1 &&
        PQsendQueryParams(c, "COMMIT", 0, NULL, NULL, NULL, NULL, 0) == 1 &&
        PQpipelineSync(c) == 1;

    cJSON *rows = NULL;
    int pre_error = 0;   /* BEGIN or set_config failed → 500 tenant-context */

    if (queued) {
        /* Drain in command order: BEGIN(0), set_config(1), query(2), COMMIT(3),
         * then PGRES_PIPELINE_SYNC. Each command's results end at a NULL. */
        int cmd = 0, sync_seen = 0;
        while (!sync_seen) {
            PGresult *r = PQgetResult(c);
            if (r == NULL) { cmd++; continue; }          /* end of command `cmd` */
            ExecStatusType st = PQresultStatus(r);
            if (st == PGRES_PIPELINE_SYNC) { PQclear(r); sync_seen = 1; continue; }
            if (cmd == 0 || cmd == 1) {                  /* BEGIN / set_config */
                if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) pre_error = 1;
            } else if (cmd == 2) {                       /* the actual query */
                if (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK) {
                    rows = t ? pgf_rows_to_json(r, t) : pgf_result_to_json(r);
                } else if (st == PGRES_PIPELINE_ABORTED) {
                    pre_error = 1;                       /* an earlier command failed */
                } else {
                    const char *ss = PQresultErrorField(r, PG_DIAG_SQLSTATE);
                    *http = map_sqlstate(ss);
                    if (errmsg) snprintf(errmsg, errlen, "%s",
                                         (*http < 500) ? sqlstate_message(ss) : "query failed");
                    LOG_ERROR("query failed [%s]: %s | sql=%s", ss ? ss : "?",
                              PQresultErrorMessage(r), q->sql);
                }
            }
            PQclear(r);
        }
    } else {
        pre_error = 1;   /* a send failed (connection likely broken) */
    }

    /* Drain any trailing results (incl. the NULL after the sync) and leave
     * non-pipeline mode so the connection is reusable. */
    PGresult *tail;
    while ((tail = PQgetResult(c)) != NULL) PQclear(tail);
    PQexitPipelineMode(c);

    if (pre_error) {
        *http = 500;
        if (errmsg) snprintf(errmsg, errlen, "tenant context failed");
        if (rows) { cJSON_Delete(rows); rows = NULL; }
    }
    if (queued) pgf_metric_inc(PGF_M_DB_PIPELINE);
    *out = rows;
    return 1;
}
#endif /* LIBPQ_HAS_PIPELINING */

static cJSON *run_rows(const pgf_query_t *q, const pgf_table_t *t, const char *rls_tenant,
                       int *http, char *errmsg, size_t errlen) {
    PGconn *c = db_connection_acquire();
    if (!c) { *http = 500; if (errmsg) snprintf(errmsg, errlen, "database unavailable"); return NULL; }

#ifdef LIBPQ_HAS_PIPELINING
    /* Opt-in: collapse the pooled-mode 4 round trips into one. Only the pooled
     * (tenant-scoped) path benefits — single-tenant is already one round trip. */
    if (rls_tenant && db_connection_pipeline_enabled()) {
        cJSON *out = NULL;
        if (run_rows_pipelined(c, q, t, rls_tenant, &out, http, errmsg, errlen)) {
            db_connection_release(c);
            return out;
        }
        /* declined before sending anything → fall through to the sequential path */
    }
#endif

    int in_txn = 0;
    if (rls_tenant) {
        const char *p[1] = { rls_tenant };
        PGresult *b = PQexec(c, "BEGIN");
        int ok = (PQresultStatus(b) == PGRES_COMMAND_OK); PQclear(b);
        if (ok) {
            PGresult *s = db_connection_exec_cached(
                c, "SELECT set_config('app.tenant_id', $1, true)", 1, p);
            ok = (PQresultStatus(s) == PGRES_TUPLES_OK); PQclear(s);
        }
        if (!ok) {
            PGresult *rb = PQexec(c, "ROLLBACK"); PQclear(rb);
            db_connection_release(c);
            *http = 500; if (errmsg) snprintf(errmsg, errlen, "tenant context failed");
            return NULL;
        }
        in_txn = 1;
    }

    PGresult *r = db_connection_exec_cached(c, q->sql, q->nparams,
                                            (const char *const *)q->params);
    cJSON *rows = NULL;
    ExecStatusType st = PQresultStatus(r);
    int success = (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK);
    if (success) {
        /* t == NULL: a table-less result (RPC) — type columns by result OID. */
        rows = t ? pgf_rows_to_json(r, t) : pgf_result_to_json(r);   /* COMMAND_OK => empty set */
    } else {
        const char *ss = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        *http = map_sqlstate(ss);
        /* M-7: client gets a generic per-category message, never the raw libpq
         * primary (schema disclosure / cross-tenant existence oracle). The full
         * diagnostic is logged below for operators. */
        if (errmsg) snprintf(errmsg, errlen, "%s",
                             (*http < 500) ? sqlstate_message(ss) : "query failed");
        LOG_ERROR("query failed [%s]: %s | sql=%s", ss ? ss : "?",
                  PQresultErrorMessage(r), q->sql);
    }
    PQclear(r);

    /* Always end the transaction before returning the connection to the pool —
     * db_connection_release does not reset connection state. */
    if (in_txn) { PGresult *e = PQexec(c, success ? "COMMIT" : "ROLLBACK"); PQclear(e); }
    db_connection_release(c);
    return rows;
}

/* Assemble the row-level scope (the set of AND-ed column=value constraints) for
 * (table, action, caller). Today that is at most the owner rule; tenant scoping
 * (Phase 7 pooled mode) will add a second rule here without touching callers.
 * Returns -1 if a configured scope column does not exist (policy misconfig). */
/* A charset-restricted identifier ([A-Za-z_][A-Za-z0-9_]*), for the VIA
 * relationship identifiers that name a table we can't catalog-validate. Combined
 * with quoting in the builder, this keeps them injection-safe. */
static bool is_safe_ident(const char *s) {
    if (!s || !*s) return false;
    if (!isalpha((unsigned char)*s) && *s != '_') return false;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    return true;
}

static int make_scope(const pgf_table_t *t, pgf_action_t action,
                      const pgf_identity_t *who, pgf_scope_t *scope) {
    memset(scope, 0, sizeof *scope);   /* every rule slot starts EQ/zeroed */
    scope->count = 0;

    /* Tenant scope (pooled mode only): any catalog table that carries the tenant
     * column is confined to the caller's tenant. The global platform_admin is
     * exempt (it manages every tenant). A non-platform caller with no tenant in
     * pooled mode is a misconfiguration — deny rather than risk a cross-tenant
     * leak. In single-tenant mode pgf_tenancy_column() is NULL, so this is a
     * no-op and the produced SQL is identical to before (guarded by the
     * query_builder regression test). */
    const char *tcol = pgf_tenancy_column();
    if (tcol && pgf_table_column(t, tcol) && strcmp(who->role, "platform_admin") != 0) {
        if (!who->tenant_id[0]) return -1;
        scope->rule[scope->count].column = tcol;
        scope->rule[scope->count].value  = who->tenant_id;
        scope->count++;
    }

    /* Owner scope (row-level ownership, within the tenant). May be a single
     * column (EQ), any of several (OR), or membership in a related table (VIA). */
    pgf_owner_spec_t os;
    if (pgf_policy_owner_scope(t->name, action, who->role, &os)) {
        /* SECURITY (H-9): OR (owner_any) / VIA (owner_via) are read filters — they
         * cannot be enforced on INSERT (no single column to force, no membership to
         * assert at create time). A create policy using them would leave ownership
         * client-controlled (owner spoofing) or the row unowned, so reject the
         * misconfiguration (-> 500) rather than silently create a spoofable row.
         * Only EQ (owner_column) is forceable on create. */
        if (action == PGF_ACT_CREATE && os.kind != PGF_OWNER_EQ) return -1;
        if (scope->count >= PGF_MAX_SCOPE) return -1;
        pgf_scope_rule_t *r = &scope->rule[scope->count];
        r->value = who->user_id;
        switch (os.kind) {
        case PGF_OWNER_EQ:
            if (!pgf_table_column(t, os.column)) return -1;
            r->kind = PGF_SCOPE_EQ; r->column = os.column;
            break;
        case PGF_OWNER_ANY:
            if (os.ncolumns < 1 || os.ncolumns > PGF_MAX_OR) return -1;
            for (int i = 0; i < os.ncolumns; i++) {
                if (!pgf_table_column(t, os.columns[i])) return -1;
                r->cols[i] = os.columns[i];
            }
            r->kind = PGF_SCOPE_OR; r->ncols = os.ncolumns;
            break;
        case PGF_OWNER_VIA:
            /* local is a column on this table; the related identifiers can't be
             * catalog-checked here, so charset-validate them (then quoted). */
            if (!pgf_table_column(t, os.via.local)) return -1;
            if (!is_safe_ident(os.via.table) || !is_safe_ident(os.via.ref) ||
                !is_safe_ident(os.via.user)) return -1;
            r->kind = PGF_SCOPE_VIA;
            r->via_table = os.via.table; r->via_ref   = os.via.ref;
            r->via_local = os.via.local; r->via_user  = os.via.user;
            break;
        default:
            return -1;
        }
        scope->count++;
    }
    return 0;
}

/* Emit a realtime change event after a successful write. Demand-gated: does
 * nothing unless some client is subscribed AND the table is realtime-enabled, so
 * the write path pays nothing when nobody is listening / realtime is off. */
static void rt_emit(const char *table, pgf_action_t action, const cJSON *row) {
    if (!pgf_realtime_active()) return;
    if (!pgf_policy_realtime_enabled(table)) return;
    const char *op = action == PGF_ACT_CREATE ? "INSERT"
                   : action == PGF_ACT_UPDATE ? "UPDATE"
                   : action == PGF_ACT_DELETE ? "DELETE" : "?";
    pgf_realtime_publish(table, op, row);
}

/* Shared shape for the write builders: build -> run -> {status, row}. */
typedef int (*build_fn)(const pgf_table_t *, const cJSON *, const pgf_scope_t *,
                        pgf_query_t *, char *, size_t);

static pgf_api_result_t run_write(const pgf_identity_t *who, const cJSON *req,
                                  build_fn build, pgf_action_t action,
                                  int ok_status, int require_row) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const pgf_table_t *t = resolve_table(req);
    if (!t) return result_error(404, "unknown table");
    if (!pgf_policy_allows(t->name, action, who->role)) return result_error(403, "forbidden");

    pgf_scope_t scope;
    if (make_scope(t, action, who, &scope) != 0) return result_error(500, "policy misconfiguration");

    char err[256] = {0};
    pgf_query_t q;
    if (build(t, req, &scope, &q, err, sizeof err) != 0) return result_error(400, err);

    int http = 200;
    char emsg[256] = {0};
    cJSON *rows = run_rows(&q, t, rls_tenant_setting(who), &http, emsg, sizeof emsg);
    pgf_query_free(&q);
    if (!rows) return result_error(http, emsg[0] ? emsg : "query failed");

    int found = cJSON_GetArraySize(rows) > 0;
    cJSON *row = found ? cJSON_DetachItemFromArray(rows, 0) : NULL;
    cJSON_Delete(rows);
    if (require_row && !found) return result_error(404, "not found");

    if (row) rt_emit(t->name, action, row);   /* realtime change event (in-process) */

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    if (row) cJSON_AddItemToObject(o, "row", row);
    pgf_api_result_t r = { o, ok_status };
    return r;
}

/* ---- relationship embedding (richer read) ---------------------------------- */

typedef struct {
    int   to_many;             /* 0 = to-one (forward FK); 1 = to-many (reverse FK) */
    const pgf_table_t *remote; /* the related table to pull in */
    const char *local_key;     /* join column on the base table */
    const char *remote_key;    /* join column on the remote table */
} pgf_relation_t;

/* Resolve an embed name (a RELATED TABLE NAME) against the catalog FK graph: a
 * forward FK on the base table (base.col -> name.pk) is to-one; a reverse FK
 * (name.col -> base.pk) is to-many. Ambiguity (more than one FK either way) is
 * rejected — never a silent guess. Returns an HTTP status (0 == resolved). */
static int resolve_relation(const pgf_table_t *base, const char *name,
                            pgf_relation_t *out, char *err, size_t errlen) {
    const pgf_table_t *remote = pgf_catalog_find(pgf_catalog_active(), name);
    if (!remote) { snprintf(err, errlen, "no relation '%s' on '%s'", name, base->name); return 400; }

    const pgf_column_t *fk = NULL; int nfk = 0;          /* to-one: forward FK on base */
    for (int i = 0; i < base->ncols; i++)
        if (base->cols[i].is_fk && !strcmp(base->cols[i].fk_table, name)) { fk = &base->cols[i]; nfk++; }
    if (nfk > 1) { snprintf(err, errlen, "ambiguous relation '%s' (multiple FKs)", name); return 400; }
    if (nfk == 1) {
        out->to_many = 0; out->remote = remote;
        out->local_key = fk->name; out->remote_key = fk->fk_column;
        return 0;
    }

    if (base->pk_index < 0) { snprintf(err, errlen, "no relation '%s' on '%s'", name, base->name); return 400; }
    const pgf_column_t *rfk = NULL; int nrfk = 0;        /* to-many: reverse FK on remote */
    for (int i = 0; i < remote->ncols; i++)
        if (remote->cols[i].is_fk && !strcmp(remote->cols[i].fk_table, base->name)) { rfk = &remote->cols[i]; nrfk++; }
    if (nrfk > 1) { snprintf(err, errlen, "ambiguous relation '%s' (multiple FKs)", name); return 400; }
    if (nrfk == 1) {
        out->to_many = 1; out->remote = remote;
        out->local_key = base->cols[base->pk_index].name; out->remote_key = rfk->name;
        return 0;
    }
    snprintf(err, errlen, "no relation '%s' on '%s'", name, base->name);
    return 400;
}

/* Canonical comparable text for a scalar join value ("\"uuid\"" / "7"); NULL for
 * a JSON null / non-scalar. Both sides of the join serialize the same pg type the
 * same way, so canonical forms match exactly. Caller frees. */
static char *join_key(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsObject(v) || cJSON_IsArray(v)) return NULL;
    return cJSON_PrintUnformatted(v);
}

/* Embed one relation: re-run the SAME authz (role + row scope) on the related
 * table, fetch its rows by `remote_key IN (base local keys)`, and stitch them
 * onto each base row under the relation name (object for to-one, array for
 * to-many). One query per relation — not N+1. Returns an HTTP status (0 == ok). */
static int embed_one(const pgf_identity_t *who, const pgf_table_t *base, cJSON *rows,
                     const char *name, const pgf_table_t **out_remote, bool *out_to_many,
                     char *err, size_t errlen) {
    pgf_relation_t rel;
    int rc = resolve_relation(base, name, &rel, err, errlen);
    if (rc) return rc;
    *out_remote = rel.remote;        /* so a dotted path can recurse into these rows */
    *out_to_many = rel.to_many;

    /* The related table is read on the caller's behalf — it must pass the same
     * read policy a direct LIST would. No leaking related rows you can't see. */
    if (!pgf_policy_allows(rel.remote->name, PGF_ACT_LIST, who->role)) {
        snprintf(err, errlen, "forbidden: cannot read '%s'", rel.remote->name);
        return 403;
    }

    cJSON *in = cJSON_CreateArray();                     /* distinct non-null local keys */
    cJSON *brow;
    cJSON_ArrayForEach(brow, rows) {
        const cJSON *kv = cJSON_GetObjectItemCaseSensitive(brow, rel.local_key);
        char *k = join_key(kv);
        if (!k) continue;
        int seen = 0; const cJSON *e;
        cJSON_ArrayForEach(e, in) { char *ek = join_key(e); int m = ek && !strcmp(ek, k); free(ek); if (m) { seen = 1; break; } }
        if (!seen) cJSON_AddItemToArray(in, cJSON_Duplicate(kv, 1));
        free(k);
    }

    if (cJSON_GetArraySize(in) == 0) {                   /* nothing to fetch */
        cJSON_Delete(in);
        cJSON_ArrayForEach(brow, rows)
            cJSON_AddItemToObject(brow, name, rel.to_many ? cJSON_CreateArray() : cJSON_CreateNull());
        return 0;
    }

    cJSON *sreq = cJSON_CreateObject();                  /* synthetic scoped LIST */
    cJSON_AddStringToObject(sreq, "table", rel.remote->name);
    cJSON *cond = cJSON_AddObjectToObject(cJSON_AddObjectToObject(sreq, "where"), rel.remote_key);
    cJSON_AddItemToObject(cond, "in", in);               /* takes ownership of `in` */
    cJSON_AddNumberToObject(sreq, "limit", PGF_LIST_MAX_LIMIT);

    pgf_scope_t scope;
    if (make_scope(rel.remote, PGF_ACT_LIST, who, &scope) != 0) {
        cJSON_Delete(sreq); snprintf(err, errlen, "policy misconfiguration"); return 500;
    }
    pgf_query_t q; char qerr[256] = {0};
    if (pgf_build_list(rel.remote, sreq, &scope, NULL, &q, qerr, sizeof qerr) != 0) {
        cJSON_Delete(sreq); snprintf(err, errlen, "%s", qerr); return 400;
    }
    int http = 200; char emsg[256] = {0};
    cJSON *related = run_rows(&q, rel.remote, rls_tenant_setting(who), &http, emsg, sizeof emsg);
    pgf_query_free(&q);
    cJSON_Delete(sreq);
    if (!related) { snprintf(err, errlen, "%s", emsg[0] ? emsg : "embed query failed"); return http; }

    cJSON_ArrayForEach(brow, rows) {                     /* stitch matches onto base rows */
        char *k = join_key(cJSON_GetObjectItemCaseSensitive(brow, rel.local_key));
        if (rel.to_many) {
            cJSON *arr = cJSON_CreateArray();
            if (k) {
                const cJSON *e;
                cJSON_ArrayForEach(e, related) {
                    char *ek = join_key(cJSON_GetObjectItemCaseSensitive(e, rel.remote_key));
                    if (ek && !strcmp(ek, k)) cJSON_AddItemToArray(arr, cJSON_Duplicate(e, 1));
                    free(ek);
                }
            }
            cJSON_AddItemToObject(brow, name, arr);
        } else {
            cJSON *match = NULL;
            if (k) {
                const cJSON *e;
                cJSON_ArrayForEach(e, related) {
                    char *ek = join_key(cJSON_GetObjectItemCaseSensitive(e, rel.remote_key));
                    int hit = ek && !strcmp(ek, k); free(ek);
                    if (hit) { match = cJSON_Duplicate(e, 1); break; }
                }
            }
            cJSON_AddItemToObject(brow, name, match ? match : cJSON_CreateNull());
        }
        free(k);
    }
    cJSON_Delete(related);
    return 0;
}

#define PGF_MAX_EMBED_DEPTH 4

/* Embed a (possibly dotted) relation path like "order_items.product": embed the
 * first relation into `rows`, then recurse into the just-embedded rows for the
 * rest. Each level re-runs the caller's authz + row scope (embed_one). Returns an
 * HTTP status (0 == ok). */
static int embed_path(const pgf_identity_t *who, const pgf_table_t *base, cJSON *rows,
                      const char *path, int depth, char *err, size_t errlen) {
    if (depth > PGF_MAX_EMBED_DEPTH) { snprintf(err, errlen, "embed nested too deeply"); return 400; }

    char first[64];
    const char *dot = strchr(path, '.');
    size_t flen = dot ? (size_t)(dot - path) : strlen(path);
    if (flen == 0 || flen >= sizeof first) { snprintf(err, errlen, "bad embed path"); return 400; }
    memcpy(first, path, flen);
    first[flen] = '\0';
    const char *rest = dot ? dot + 1 : NULL;

    const pgf_table_t *remote = NULL;
    bool to_many = false;
    int rc = embed_one(who, base, rows, first, &remote, &to_many, err, errlen);
    if (rc || !rest || !*rest) return rc;

    /* Gather the just-embedded rows BY REFERENCE so the deeper embed mutates the
     * same objects in place (the reference array frees only itself, not the rows). */
    cJSON *gathered = cJSON_CreateArray();
    cJSON *brow;
    cJSON_ArrayForEach(brow, rows) {
        cJSON *att = cJSON_GetObjectItemCaseSensitive(brow, first);
        if (to_many) {
            if (cJSON_IsArray(att)) {
                cJSON *it;
                cJSON_ArrayForEach(it, att) cJSON_AddItemReferenceToArray(gathered, it);
            }
        } else if (cJSON_IsObject(att)) {
            cJSON_AddItemReferenceToArray(gathered, att);
        }
    }
    rc = embed_path(who, remote, gathered, rest, depth + 1, err, errlen);
    cJSON_Delete(gathered);
    return rc;
}

/* Decode an opaque cursor token into the array of the previous page's key values.
 * An empty token is the first keyset page (empty array). Returns NULL + *bad on a
 * malformed token. Caller owns the result. */
static cJSON *decode_cursor(const char *token, bool *bad) {
    *bad = false;
    if (!*token) return cJSON_CreateArray();
    unsigned char buf[1024];
    size_t n = 0;
    if (pgf_b64url_decode(token, strlen(token), buf, sizeof buf - 1, &n) != 0) { *bad = true; return NULL; }
    buf[n] = '\0';
    cJSON *arr = cJSON_Parse((char *)buf);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); *bad = true; return NULL; }
    return arr;
}

/* Build the next-page cursor token from the last row's sort-key values. */
static char *encode_next_cursor(const pgf_table_t *t, const cJSON *req, const cJSON *last_row) {
    pgf_sortkey_t keys[PGF_MAX_SORTKEYS];
    char e[128] = {0};
    int nk = pgf_resolve_sortkeys(t, req, keys, PGF_MAX_SORTKEYS, e, sizeof e);
    if (nk < 0) return NULL;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < nk; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(last_row, keys[i].column);
        cJSON_AddItemToArray(arr, v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull());
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) return NULL;
    size_t cap = (strlen(json) * 4) / 3 + 8;
    char *tok = malloc(cap);
    if (tok && pgf_b64url_encode((const unsigned char *)json, strlen(json), tok, cap) != 0) { free(tok); tok = NULL; }
    free(json);
    return tok;
}

pgf_api_result_t pgf_api_list(const pgf_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const pgf_table_t *t = resolve_table(req);
    if (!t) return result_error(404, "unknown table");
    if (!pgf_policy_allows(t->name, PGF_ACT_LIST, who->role)) return result_error(403, "forbidden");

    pgf_scope_t scope;
    if (make_scope(t, PGF_ACT_LIST, who, &scope) != 0) return result_error(500, "policy misconfiguration");

    /* Aggregate mode: { group?, aggregate? } -> GROUP BY rollups under the same
     * filters + row scope (so totals never include rows the caller can't see). A
     * different result shape, so it short-circuits the row/embed/cursor path. */
    const cJSON *grp = cJSON_GetObjectItemCaseSensitive(req, "group");
    const cJSON *agg = cJSON_GetObjectItemCaseSensitive(req, "aggregate");
    if ((cJSON_IsArray(grp) && cJSON_GetArraySize(grp) > 0) ||
        (cJSON_IsArray(agg) && cJSON_GetArraySize(agg) > 0)) {
        char aerr[256] = {0};
        pgf_query_t aq;
        if (pgf_build_aggregate(t, req, &scope, &aq, aerr, sizeof aerr) != 0)
            return result_error(400, aerr);
        int ahttp = 200; char aemsg[256] = {0};
        cJSON *arows = run_rows(&aq, NULL, rls_tenant_setting(who), &ahttp, aemsg, sizeof aemsg);
        pgf_query_free(&aq);
        if (!arows) return result_error(ahttp, aemsg[0] ? aemsg : "query failed");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(arows));
        cJSON_AddItemToObject(o, "rows", arows);
        pgf_api_result_t r = { o, 200 };
        return r;
    }

    /* Keyset mode when a `cursor` is present (a token string; "" = first page). */
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(req, "cursor");
    bool keyset = cJSON_IsString(cur);
    cJSON *cursor_vals = NULL;
    if (keyset) {
        bool bad = false;
        cursor_vals = decode_cursor(cur->valuestring, &bad);
        if (bad) return result_error(400, "invalid cursor");
    }

    char err[256] = {0};
    pgf_query_t q;
    int brc = pgf_build_list(t, req, &scope, cursor_vals, &q, err, sizeof err);
    cJSON_Delete(cursor_vals);   /* values are copied into the query params */
    if (brc != 0) return result_error(400, err);

    int http = 200;
    char emsg[256] = {0};
    cJSON *rows = run_rows(&q, t, rls_tenant_setting(who), &http, emsg, sizeof emsg);
    pgf_query_free(&q);
    if (!rows) return result_error(http, emsg[0] ? emsg : "query failed");

    /* Relationship embedding: { "embed": ["categories", ...] } — each name is a
     * related table; every embedded read re-runs the caller's authz + row scope. */
    const cJSON *embed = cJSON_GetObjectItemCaseSensitive(req, "embed");
    if (cJSON_IsArray(embed)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, embed) {
            if (!cJSON_IsString(e)) { cJSON_Delete(rows); return result_error(400, "embed entries must be strings"); }
            char eerr[256] = {0};
            int erc = embed_path(who, t, rows, e->valuestring, 1, eerr, sizeof eerr);
            if (erc) { cJSON_Delete(rows); return result_error(erc, eerr[0] ? eerr : "embed failed"); }
        }
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(rows));   /* rows in this page */

    /* Exact total (for pagination UIs): { "count": "exact" } -> "total", the size
     * of the unpaginated result under the same filters + scope. */
    const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(req, "count");
    if (cJSON_IsString(cnt) && !strcasecmp(cnt->valuestring, "exact")) {
        pgf_query_t cq; char cerr[256] = {0};
        if (pgf_build_count(t, req, &scope, &cq, cerr, sizeof cerr) == 0) {
            int chttp = 200; char cemsg[256] = {0};
            cJSON *cr = run_rows(&cq, NULL, rls_tenant_setting(who), &chttp, cemsg, sizeof cemsg);
            pgf_query_free(&cq);
            if (cr) {
                cJSON *first = cJSON_GetArrayItem(cr, 0);
                cJSON *cv = first ? cJSON_GetObjectItemCaseSensitive(first, "count") : NULL;
                double total = 0;
                if (cJSON_IsNumber(cv)) total = cv->valuedouble;
                else if (cJSON_IsString(cv) && cv->valuestring) total = strtod(cv->valuestring, NULL);
                cJSON_AddNumberToObject(o, "total", total);
                cJSON_Delete(cr);
            }
        }
    }

    /* Keyset: hand back a next_cursor when the page came back full (more may exist). */
    if (keyset) {
        int page = cJSON_GetArraySize(rows);
        long limit = PGF_LIST_DEFAULT_LIMIT;
        const cJSON *jl = cJSON_GetObjectItemCaseSensitive(req, "limit");
        if (cJSON_IsNumber(jl)) {
            limit = (long)jl->valuedouble;
            if (limit < 1) limit = 1;
            if (limit > PGF_LIST_MAX_LIMIT) limit = PGF_LIST_MAX_LIMIT;
        }
        if (page >= limit) {
            char *nc = encode_next_cursor(t, req, cJSON_GetArrayItem(rows, page - 1));
            if (nc) { cJSON_AddStringToObject(o, "next_cursor", nc); free(nc); }
        }
    }

    cJSON_AddItemToObject(o, "rows", rows);
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_get(const pgf_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const pgf_table_t *t = resolve_table(req);
    if (!t) return result_error(404, "unknown table");
    if (!pgf_policy_allows(t->name, PGF_ACT_GET, who->role)) return result_error(403, "forbidden");

    pgf_scope_t scope;
    if (make_scope(t, PGF_ACT_GET, who, &scope) != 0) return result_error(500, "policy misconfiguration");

    char err[256] = {0};
    pgf_query_t q;
    if (pgf_build_get(t, req, &scope, &q, err, sizeof err) != 0) return result_error(400, err);

    int http = 200;
    char emsg[256] = {0};
    cJSON *rows = run_rows(&q, t, rls_tenant_setting(who), &http, emsg, sizeof emsg);
    pgf_query_free(&q);
    if (!rows) return result_error(http, emsg[0] ? emsg : "query failed");

    if (cJSON_GetArraySize(rows) == 0) { cJSON_Delete(rows); return result_error(404, "not found"); }
    cJSON *row = cJSON_DetachItemFromArray(rows, 0);
    cJSON_Delete(rows);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddItemToObject(o, "row", row);
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_create(const pgf_identity_t *who, const cJSON *req) {
    return run_write(who, req, pgf_build_create, PGF_ACT_CREATE, 201, 0);
}
pgf_api_result_t pgf_api_update(const pgf_identity_t *who, const cJSON *req) {
    return run_write(who, req, pgf_build_update, PGF_ACT_UPDATE, 200, 1);
}
pgf_api_result_t pgf_api_delete(const pgf_identity_t *who, const cJSON *req) {
    return run_write(who, req, pgf_build_delete, PGF_ACT_DELETE, 200, 1);
}

pgf_api_result_t pgf_api_rpc(const pgf_identity_t *who, const cJSON *req) {
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(req, "fn");
    if (!cJSON_IsString(fn) || !fn->valuestring[0]) return result_error(400, "fn required");
    /* Deny-by-default whitelist (a non-whitelisted fn is unreachable by anyone). */
    if (!pgf_policy_rpc_allows(fn->valuestring, who->role)) return result_error(403, "forbidden");

    /* SECURITY (M-3): fail closed on an empty tenant context. Unlike the data
     * paths, run_rows here can't lean on make_scope's seatbelt — so an anon or
     * tenant-less caller (allowed by an "anon" _rpc entry) would otherwise bind
     * app.tenant_id='' and execute the function in an undefined RLS context. In
     * pooled mode every RPC must run within a concrete tenant; platform_admin
     * ("*") is the global exception. Single-tenant mode (no tenancy column) is
     * unaffected — no RLS context is needed there. Mirrors make_scope (api.c). */
    if (pgf_tenancy_column() && strcmp(who->role, "platform_admin") != 0 && !who->tenant_id[0])
        return result_error(403, "forbidden");

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "args");   /* optional object */

    char err[256] = {0};
    pgf_query_t q;
    if (pgf_build_rpc(fn->valuestring, args, &q, err, sizeof err) != 0)
        return result_error(400, err);

    int http = 200;
    char emsg[256] = {0};
    /* table = NULL -> table-less result; runs inside the caller's tenant context. */
    cJSON *rows = run_rows(&q, NULL, rls_tenant_setting(who), &http, emsg, sizeof emsg);
    pgf_query_free(&q);
    if (!rows) return result_error(http, emsg[0] ? emsg : "rpc failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddItemToObject(o, "result", rows);
    pgf_api_result_t r = { o, 200 };
    return r;
}

void pgf_rpc_audit_security_definer(void) {
    /* H-4: a whitelisted RPC function defined SECURITY DEFINER executes as its
     * (often privileged) owner and BYPASSES row-level security. The RPC path
     * relies on RLS as its only tenant boundary, so in pooled/multi-tenant mode
     * such a function can read/write across tenants. We can't safely rewrite the
     * operator's function, so warn loudly at startup (louder in pooled mode) so
     * the misconfiguration is visible — the function should be SECURITY INVOKER,
     * or enforce tenant scope itself. */
    const char *names[64];
    int n = pgf_policy_rpc_names(names, 64);
    if (n == 0) return;
    PGconn *c = db_connection_acquire();
    if (!c) return;
    bool pooled = pgf_tenancy_column() != NULL;
    for (int i = 0; i < n; i++) {
        const char *p[1] = { names[i] };
        PGresult *r = PQexecParams(c,
            "SELECT 1 FROM pg_proc WHERE proname=$1 AND prosecdef LIMIT 1",
            1, NULL, p, NULL, NULL, 0);
        bool secdef = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1);
        PQclear(r);
        if (!secdef) continue;
        if (pooled)
            LOG_WARN("rpc: whitelisted function '%s' is SECURITY DEFINER — it bypasses "
                     "row-level security and can cross TENANT boundaries in pooled mode. "
                     "Make it SECURITY INVOKER, or have it enforce tenant scope itself.",
                     names[i]);
        else
            LOG_WARN("rpc: whitelisted function '%s' is SECURITY DEFINER — it runs as its "
                     "owner and bypasses RLS; safe only if it does its own authorization.",
                     names[i]);
    }
    db_connection_release(c);
}

pgf_api_result_t pgf_api_schema(const pgf_identity_t *who) {
    if (!who->authenticated) return result_error(401, "authentication required");
    cJSON *cat = pgf_catalog_to_cjson(pgf_catalog_active());
    cJSON_AddStringToObject(cat, "status", "ok");
    pgf_api_result_t r = { cat, 200 };
    return r;
}

pgf_api_result_t pgf_api_login(const cJSON *req) {
    const cJSON *email = cJSON_GetObjectItemCaseSensitive(req, "email");
    const cJSON *pass  = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(email) || !cJSON_IsString(pass))
        return result_error(400, "email and password required");
    /* Reject over-length input before it reaches Argon2id (CPU-DoS guard). */
    if (strlen(pass->valuestring) > MAX_PASSWORD_LEN)
        return result_error(401, "invalid credentials");

    char token[129], challenge[129];
    pgf_user_t user;
    int rc = pgf_auth_login(email->valuestring, pass->valuestring,
                            SESSION_TTL_SECONDS, token, sizeof token,
                            challenge, sizeof challenge, &user);
    if (rc == PGF_AUTH_MFA_REQUIRED) {
        /* Factor one passed; no session yet. The client submits the challenge +
         * a TOTP code to /auth/mfa/verify to finish logging in. */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "mfa_required");
        cJSON_AddStringToObject(o, "challenge", challenge);
        pgf_api_result_t r = { o, 200 };
        return r;
    }
    /* L-1: a locked account must be indistinguishable from a wrong password or a
     * non-existent user — a 429 "account locked" (vs 401) confirms the email is
     * registered and under attack, and leaks the locked->unlocked transition. Both
     * return a uniform 401. (Lockout still throttles server-side; the account just
     * isn't advertised as locked. Trade-off: a locked legitimate user sees
     * "invalid credentials" rather than a lockout notice.) */
    if (rc == PGF_AUTH_LOCKED || rc == PGF_AUTH_INVALID)
        return result_error(401, "invalid credentials");
    if (rc != PGF_AUTH_OK)      return result_error(500, "server error");

    return session_result(token, &user);
}

/* Shared {status, token, user} body for a completed login (password path or the
 * MFA second step). */
static pgf_api_result_t session_result(const char *token, const pgf_user_t *user) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "token", token);
    cJSON *u = cJSON_AddObjectToObject(o, "user");
    cJSON_AddStringToObject(u, "id", user->id);
    cJSON_AddStringToObject(u, "email", user->email);
    cJSON_AddStringToObject(u, "role", user->role);
    cJSON_AddBoolToObject(u, "email_verified", user->email_verified);
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_mfa_enroll(const pgf_identity_t *who) {
    if (!who->authenticated) return result_error(401, "authentication required");
    char secret[64], uri[256];
    int rc = pgf_mfa_enroll(who->user_id, secret, sizeof secret, uri, sizeof uri);
    if (rc == PGF_MFA_DISABLED) return result_error(403, "two-factor auth is not enabled");
    if (rc == PGF_MFA_ALREADY)  return result_error(409, "already enrolled");
    if (rc != PGF_MFA_OK)       return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "secret", secret);          /* show once, for the QR */
    cJSON_AddStringToObject(o, "otpauth_uri", uri);
    pgf_api_result_t r = { o, 200 };
    return r;
}

/* The 6-digit code field, shared by confirm/disable. */
static const char *mfa_code(const cJSON *req) {
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(req, "code");
    return cJSON_IsString(code) ? code->valuestring : NULL;
}

/* The one-time recovery codes as a JSON array (returned once at confirm/regen). */
static cJSON *recovery_codes_json(char codes[][PGF_MFA_RECOVERY_LEN]) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < PGF_MFA_RECOVERY_N; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(codes[i]));
    return arr;
}

pgf_api_result_t pgf_api_mfa_confirm(const pgf_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const char *code = mfa_code(req);
    if (!code) return result_error(400, "code required");
    char codes[PGF_MFA_RECOVERY_N][PGF_MFA_RECOVERY_LEN];
    int rc = pgf_mfa_confirm(who->user_id, code, codes);
    if (rc == PGF_MFA_DISABLED)     return result_error(403, "two-factor auth is not enabled");
    if (rc == PGF_MFA_NOT_ENROLLED) return result_error(400, "no pending enrollment");
    if (rc == PGF_MFA_INVALID)      return result_error(401, "invalid code");
    if (rc != PGF_MFA_OK)           return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "confirmed");
    cJSON_AddItemToObject(o, "recovery_codes", recovery_codes_json(codes));  /* show once */
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_mfa_recovery(const pgf_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const char *code = mfa_code(req);
    if (!code) return result_error(400, "code required");
    char codes[PGF_MFA_RECOVERY_N][PGF_MFA_RECOVERY_LEN];
    int rc = pgf_mfa_regenerate_recovery(who->user_id, code, codes);
    if (rc == PGF_MFA_NOT_ENROLLED) return result_error(400, "not enrolled");
    if (rc == PGF_MFA_INVALID)      return result_error(401, "invalid code");
    if (rc != PGF_MFA_OK)           return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddItemToObject(o, "recovery_codes", recovery_codes_json(codes));
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_mfa_disable(const pgf_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const char *code = mfa_code(req);
    if (!code) return result_error(400, "code required");
    int rc = pgf_mfa_disable(who->user_id, code);
    if (rc == PGF_MFA_NOT_ENROLLED) return result_error(400, "not enrolled");
    if (rc == PGF_MFA_INVALID)      return result_error(401, "invalid code");
    if (rc != PGF_MFA_OK)           return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "disabled");
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_mfa_verify(const cJSON *req) {
    const cJSON *challenge = cJSON_GetObjectItemCaseSensitive(req, "challenge");
    const cJSON *code      = cJSON_GetObjectItemCaseSensitive(req, "code");
    if (!cJSON_IsString(challenge) || !cJSON_IsString(code))
        return result_error(400, "challenge and code required");

    char token[129];
    pgf_user_t user;
    int rc = pgf_mfa_verify_login(challenge->valuestring, code->valuestring,
                                  SESSION_TTL_SECONDS, token, sizeof token, &user);
    if (rc == PGF_MFA_INVALID) return result_error(401, "invalid code or challenge");
    if (rc != PGF_MFA_OK)      return result_error(500, "server error");

    return session_result(token, &user);
}

/* Build + send the email-verification message for `user_id` (best-effort; no-op
 * if the mailer is off or the email is already verified). Shared by register and
 * the resend endpoint. */
static void send_email_verification(const char *user_id) {
    if (!pgf_mail_enabled()) return;
    char token[129], to[256];
    if (pgf_auth_create_email_verification(user_id, token, sizeof token, to, sizeof to) != PGF_AUTH_OK)
        return;
    const char *app = getenv("PGF_APP_URL");
    char body[1024];
    if (app && *app)
        snprintf(body, sizeof body,
            "Welcome! Please confirm your email address:\r\n%s/verify-email?token=%s\r\n\r\n"
            "This link expires in 24 hours.\r\n", app, token);
    else
        snprintf(body, sizeof body,
            "Welcome! Confirm your email address with this token (expires in 24 hours):\r\n%s\r\n",
            token);
    pgf_mail_send(to, "Verify your email", body);
}

pgf_api_result_t pgf_api_verify_email(const cJSON *req) {
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(req, "token");
    if (!cJSON_IsString(token)) return result_error(400, "token required");
    int rc = pgf_auth_verify_email(token->valuestring);
    if (rc == PGF_AUTH_INVALID) return result_error(400, "invalid or expired token");
    if (rc != PGF_AUTH_OK)      return result_error(500, "server error");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "email verified");
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_verify_email_resend(const pgf_identity_t *who) {
    if (!who->authenticated) return result_error(401, "authentication required");
    send_email_verification(who->user_id);   /* best-effort; idempotent if already verified */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "if your email is unverified, a verification link has been sent");
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_password_forgot(const cJSON *req) {
    const cJSON *email = cJSON_GetObjectItemCaseSensitive(req, "email");
    if (!cJSON_IsString(email) || !email->valuestring[0])
        return result_error(400, "email required");

    char token[129];
    if (pgf_auth_create_password_reset(email->valuestring, token, sizeof token) == PGF_AUTH_OK
        && pgf_mail_enabled()) {
        const char *app = getenv("PGF_APP_URL");
        char body[1024];
        if (app && *app)
            snprintf(body, sizeof body,
                "Someone requested a password reset for your account.\r\n\r\n"
                "Choose a new password:\r\n%s/reset-password?token=%s\r\n\r\n"
                "If you didn't request this, ignore this email. The link expires in 1 hour.\r\n",
                app, token);
        else
            snprintf(body, sizeof body,
                "Someone requested a password reset for your account.\r\n\r\n"
                "Your reset token (expires in 1 hour):\r\n%s\r\n\r\n"
                "If you didn't request this, ignore this email.\r\n", token);
        pgf_mail_send(email->valuestring, "Reset your password", body);
    }

    /* Always 200 with the same body — never reveal whether the email is registered. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "if that email is registered, a reset link has been sent");
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_password_reset(const cJSON *req) {
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(req, "token");
    const cJSON *pass  = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(token) || !cJSON_IsString(pass))
        return result_error(400, "token and password required");
    size_t plen = strlen(pass->valuestring);
    if (plen < MIN_PASSWORD_LEN) return result_error(400, "password too short (min 8 characters)");
    if (plen > MAX_PASSWORD_LEN) return result_error(400, "password too long (max 128 characters)");

    int rc = pgf_auth_perform_password_reset(token->valuestring, pass->valuestring);
    if (rc == PGF_AUTH_INVALID) return result_error(400, "invalid or expired reset token");
    if (rc != PGF_AUTH_OK)      return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "password updated");
    pgf_api_result_t r = { o, 200 };
    return r;
}

pgf_api_result_t pgf_api_oauth(const cJSON *req) {
    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(req, "provider");
    const cJSON *token    = cJSON_GetObjectItemCaseSensitive(req, "id_token");
    if (!cJSON_IsString(provider) || !cJSON_IsString(token))
        return result_error(400, "provider and id_token required");

    /* Verify the ID token (signature + iss/aud/azp/exp/iat, optional nonce) before
     * trusting any claim. A client doing the nonce dance passes the nonce it minted. */
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(req, "nonce");
    const char *exp_nonce = cJSON_IsString(nonce) ? nonce->valuestring : NULL;
    pgf_oauth_claims_t claims;
    char verr[128] = {0};
    if (pgf_oauth_verify(provider->valuestring, token->valuestring, exp_nonce, &claims, verr, sizeof verr) != 0) {
        /* L-2: don't return the verification-stage reason ("issuer mismatch",
         * "audience mismatch", "signature invalid", ...) to the client — it's a
         * stage oracle and leaks the enforced iss/aud. Log it server-side. */
        LOG_WARN("oauth: verify failed [provider=%s]: %s", provider->valuestring,
                 verr[0] ? verr : "unknown");
        return result_error(401, "invalid token");
    }

    /* Auto-provisioning, if it happens, follows the self-registration policy:
     * a new federated user gets the default signup role (if one is configured). */
    char role[32];
    const char *prole = pgf_role_default_signup(role, sizeof role) ? role : NULL;

    /* M-4: no federated self-provisioning in pooled mode — a brand-new OAuth user
     * has no tenant to bind to and would land tenant-less, slipping past the
     * pooled-mode lockdown that /auth/register enforces. An existing identity (or a
     * trusted-domain link to an existing account) still logs in; only creation of a
     * new account is refused (PGF_AUTH_INVALID -> 403). */
    if (pgf_tenancy_column()) prole = NULL;

    /* May this provider auto-link to an existing local account with this email?
     * Only if the operator trusts it for the email's domain (H-3). */
    bool link_trusted = pgf_oauth_email_link_allowed(provider->valuestring, claims.email);

    char stoken[129], challenge[129];
    pgf_user_t user;
    int rc = pgf_auth_oauth_login(provider->valuestring, claims.sub, claims.email,
                                  claims.email_verified, link_trusted, prole, SESSION_TTL_SECONDS,
                                  stoken, sizeof stoken, challenge, sizeof challenge, &user);
    if (rc == PGF_AUTH_MFA_REQUIRED) {
        /* Federated factor one passed; no session yet. Same flow as password login:
         * the client submits the challenge + a TOTP code to /auth/mfa/verify. */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "mfa_required");
        cJSON_AddStringToObject(o, "challenge", challenge);
        pgf_api_result_t r = { o, 200 };
        return r;
    }
    if (rc == PGF_AUTH_INVALID) return result_error(403, "no account for this identity");
    if (rc != PGF_AUTH_OK)      return result_error(500, "server error");
    return session_result(stoken, &user);
}

pgf_api_result_t pgf_api_create_user(const pgf_identity_t *who, const cJSON *req) {
    if (!who->authenticated)               return result_error(401, "authentication required");
    if (!pgf_role_is_superuser(who->role)) return result_error(403, "forbidden");

    const cJSON *email  = cJSON_GetObjectItemCaseSensitive(req, "email");
    const cJSON *pass   = cJSON_GetObjectItemCaseSensitive(req, "password");
    const cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
    if (!cJSON_IsString(email) || !cJSON_IsString(pass) || !email->valuestring[0])
        return result_error(400, "email and password required");
    if (!cJSON_IsString(role_j) || !role_j->valuestring[0])
        return result_error(400, "role required");
    if (strlen(pass->valuestring) < MIN_PASSWORD_LEN)
        return result_error(400, "password too short (min 8 characters)");
    if (strlen(pass->valuestring) > MAX_PASSWORD_LEN)
        return result_error(400, "password too long (max 128 characters)");
    const char *role = role_j->valuestring;

    /* Escalation boundary: platform_admin is created out-of-band only (CLI),
     * never via any authenticated request — the line that keeps the global
     * platform tier unreachable from in-band actors. Other (tenant-scoped)
     * superuser roles are fine: they stay confined by tenant scope. */
    if (!strcmp(role, "platform_admin"))
        return result_error(403, "platform_admin can only be created out-of-band");

    /* Resolve the new user's tenant (pooled mode). A tenant admin can only
     * create within its OWN tenant — never trust a client-supplied tenant_id;
     * the global platform_admin must name the target tenant explicitly. */
    const char *tenant = "";   /* empty => tenant column left unset (single-tenant) */
    if (pgf_tenancy_column()) {
        if (!strcmp(who->role, "platform_admin")) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "tenant_id");
            if (!cJSON_IsString(t) || !t->valuestring[0])
                return result_error(400, "tenant_id required");
            tenant = t->valuestring;
        } else {
            if (!who->tenant_id[0]) return result_error(403, "forbidden");
            tenant = who->tenant_id;
        }
    }

    char id[37];
    int rc = pgf_auth_create_user(email->valuestring, pass->valuestring, role, tenant,
                                  id, sizeof id);
    if (rc == PGF_AUTH_CONFLICT) return result_error(409, "email already registered");
    if (rc == PGF_AUTH_INVALID)  return result_error(400, "invalid tenant");
    if (rc != PGF_AUTH_OK)       return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON *u = cJSON_AddObjectToObject(o, "user");
    cJSON_AddStringToObject(u, "id", id);
    cJSON_AddStringToObject(u, "email", email->valuestring);
    cJSON_AddStringToObject(u, "role", role);
    if (tenant[0]) cJSON_AddStringToObject(u, "tenant_id", tenant);
    pgf_api_result_t r = { o, 201 };
    return r;
}

/* Is `user_id` a participant for `key_val` via the membership table? One query:
 * EXISTS(SELECT 1 FROM via_table WHERE via_ref = key AND via_user = caller). The
 * identifiers were charset-validated by make_scope, so they are safe to quote.
 * L-4: in pooled mode the query runs inside the caller's tenant RLS context (BEGIN
 * + set_config app.tenant_id) — exactly like run_rows — so it is neither evaluated
 * across all tenants (no tenant column) nor wrongly denied (fail-closed RLS on a
 * NULL tenant). `tenant` is "" / NULL in single-tenant mode (no transaction). */
static bool rt_membership(const char *via_table, const char *via_ref, const char *via_user,
                          const char *key_val, const char *user_id, const char *tenant) {
    PGconn *c = db_connection_acquire();
    if (!c) return false;
    bool ok = false;
    int in_txn = 0;
    if (tenant && *tenant) {
        PGresult *b = PQexec(c, "BEGIN");
        int begun = (PQresultStatus(b) == PGRES_COMMAND_OK);
        PQclear(b);
        if (begun) {
            const char *tp[1] = { tenant };
            PGresult *s = PQexecParams(c, "SELECT set_config('app.tenant_id', $1, true)",
                                       1, NULL, tp, NULL, NULL, 0);
            in_txn = (PQresultStatus(s) == PGRES_TUPLES_OK);
            PQclear(s);
        }
        if (!in_txn) { if (begun) { PGresult *e = PQexec(c, "ROLLBACK"); PQclear(e); }
                       db_connection_release(c); return false; }
    }
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT 1 FROM \"%s\" WHERE \"%s\" = $1 AND \"%s\" = $2 LIMIT 1",
             via_table, via_ref, via_user);
    const char *p[2] = { key_val, user_id };
    PGresult *res = PQexecParams(c, sql, 2, NULL, p, NULL, NULL, 0);
    ok = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1);
    PQclear(res);
    if (in_txn) { PGresult *e = PQexec(c, "COMMIT"); PQclear(e); }
    db_connection_release(c);
    return ok;
}

/* Publish-time re-authorization callback (M-5): the subscription carries the VIA
 * membership coordinates captured at subscribe; re-verify them against the live
 * membership table (with the same tenant context). */
bool pgf_api_rt_recheck_member(const pgf_subscription_t *sub) {
    if (!sub->via) return true;   /* not a membership subscription */
    return rt_membership(sub->via_table, sub->via_ref, sub->via_user,
                         sub->via_key, sub->user_id, sub->tenant);
}

int pgf_api_authorize_subscription(const pgf_identity_t *who, const cJSON *req,
                                   pgf_subscription_t *sub, char *errbuf, size_t errlen) {
    memset(sub, 0, sizeof *sub);
    if (!who->authenticated) { snprintf(errbuf, errlen, "authentication required"); return 401; }
    const pgf_table_t *t = resolve_table(req);
    if (!t) { snprintf(errbuf, errlen, "unknown table"); return 404; }
    if (!pgf_policy_allows(t->name, PGF_ACT_LIST, who->role)) {
        snprintf(errbuf, errlen, "forbidden"); return 403;
    }
    if (!pgf_policy_realtime_enabled(t->name)) {
        snprintf(errbuf, errlen, "realtime not enabled for table"); return 403;
    }
    snprintf(sub->table, sizeof sub->table, "%s", t->name);

    pgf_scope_t scope;
    if (make_scope(t, PGF_ACT_LIST, who, &scope) != 0) {
        snprintf(errbuf, errlen, "policy misconfiguration"); return 500;
    }

    /* optional { key: { column, value } } — required for a VIA (membership) rule */
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(req, "key");
    const cJSON *kc = cJSON_IsObject(key) ? cJSON_GetObjectItemCaseSensitive(key, "column") : NULL;
    const cJSON *kv = cJSON_IsObject(key) ? cJSON_GetObjectItemCaseSensitive(key, "value") : NULL;
    const char *key_col = cJSON_IsString(kc) ? kc->valuestring : NULL;
    const char *key_val = cJSON_IsString(kv) ? kv->valuestring : NULL;

    for (int i = 0; i < scope.count && sub->npreds < PGF_RT_MAX_PREDS; i++) {
        const pgf_scope_rule_t *r = &scope.rule[i];
        pgf_rt_pred_t *p = &sub->preds[sub->npreds];
        if (r->kind == PGF_SCOPE_EQ) {
            p->is_or = false;
            snprintf(p->column, sizeof p->column, "%s", r->column);
            snprintf(p->value,  sizeof p->value,  "%s", r->value);
            sub->npreds++;
        } else if (r->kind == PGF_SCOPE_OR) {
            p->is_or = true; p->ncols = r->ncols;
            for (int j = 0; j < r->ncols; j++)
                snprintf(p->cols[j], sizeof p->cols[j], "%s", r->cols[j]);
            snprintf(p->value, sizeof p->value, "%s", r->value);
            sub->npreds++;
        } else { /* PGF_SCOPE_VIA: require a key on the local column + membership */
            if (!key_col || !key_val || strcmp(key_col, r->via_local) != 0) {
                snprintf(errbuf, errlen,
                         "subscription requires key { column: \"%s\", value }", r->via_local);
                return 400;
            }
            const char *tenant = rls_tenant_setting(who);   /* NULL/"" in single-tenant */
            if (!rt_membership(r->via_table, r->via_ref, r->via_user, key_val, who->user_id,
                               tenant ? tenant : "")) {
                snprintf(errbuf, errlen, "forbidden"); return 403;
            }
            p->is_or = false;
            snprintf(p->column, sizeof p->column, "%s", r->via_local);
            snprintf(p->value,  sizeof p->value,  "%s", key_val);
            sub->npreds++;
            /* Record the membership coordinates so delivery can RE-verify membership
             * on every publish (M-5) — the flat predicate above is necessary but not
             * sufficient once membership can be revoked. */
            sub->via = true;
            snprintf(sub->via_table, sizeof sub->via_table, "%s", r->via_table);
            snprintf(sub->via_ref,   sizeof sub->via_ref,   "%s", r->via_ref);
            snprintf(sub->via_user,  sizeof sub->via_user,  "%s", r->via_user);
            snprintf(sub->via_key,   sizeof sub->via_key,   "%s", key_val);
            snprintf(sub->user_id,   sizeof sub->user_id,   "%s", who->user_id);
            snprintf(sub->tenant,    sizeof sub->tenant,    "%s", tenant ? tenant : "");
        }
    }
    return 200;
}

pgf_api_result_t pgf_api_register(const cJSON *req) {
    const cJSON *email  = cJSON_GetObjectItemCaseSensitive(req, "email");
    const cJSON *pass   = cJSON_GetObjectItemCaseSensitive(req, "password");
    const cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
    if (!cJSON_IsString(email) || !cJSON_IsString(pass) || !email->valuestring[0])
        return result_error(400, "email and password required");
    if (strlen(pass->valuestring) < MIN_PASSWORD_LEN)
        return result_error(400, "password too short (min 8 characters)");
    if (strlen(pass->valuestring) > MAX_PASSWORD_LEN)
        return result_error(400, "password too long (max 128 characters)");

    /* Pooled mode: open self-registration needs a tenant to bind the new user
     * to; resolving that (e.g. by subdomain) is a later task. Until then signup
     * is single-tenant only — provision tenant users via the tenant admin. */
    if (pgf_tenancy_column())
        return result_error(403, "self-registration is not available in pooled mode");

    /* Resolve the requested role (explicit, else the configured default), then
     * enforce the signup whitelist — deny-by-default, superusers never allowed. */
    char role[32];
    if (cJSON_IsString(role_j) && role_j->valuestring[0]) {
        snprintf(role, sizeof role, "%s", role_j->valuestring);
    } else if (!pgf_role_default_signup(role, sizeof role)) {
        return result_error(403, "self-registration is not enabled");
    }
    if (!pgf_role_can_self_register(role))
        return result_error(403, "role is not open to self-registration");

    char token[129];
    pgf_user_t user;
    int rc = pgf_auth_register(email->valuestring, pass->valuestring, role,
                               SESSION_TTL_SECONDS, token, sizeof token, &user);

    if (rc == PGF_AUTH_OK) send_email_verification(user.id);  /* only on a real new account */

    /* H-5: by default register does NOT auto-login and does NOT reveal whether the
     * email already exists — a token (or a 409) returned only for NEW emails is an
     * account-enumeration oracle. A successful new registration and an existing-
     * email conflict return the SAME uniform 202; the client signs in separately
     * via /auth/login. Operators who accept the enumeration trade-off can restore
     * auto-login (201 + token, 409 on conflict) with PGF_REGISTER_AUTOLOGIN=1.
     * (Residual: when SMTP is configured the verification email above adds latency
     * on the new-account path only — a weaker timing side-channel, not closed here.) */
    const char *al = getenv("PGF_REGISTER_AUTOLOGIN");
    bool autologin = al && (!strcmp(al, "1") || !strcmp(al, "true") || !strcmp(al, "yes"));

    if (autologin) {
        if (rc == PGF_AUTH_CONFLICT) return result_error(409, "email already registered");
        if (rc != PGF_AUTH_OK)       return result_error(500, "server error");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddStringToObject(o, "token", token);
        cJSON *u = cJSON_AddObjectToObject(o, "user");
        cJSON_AddStringToObject(u, "id", user.id);
        cJSON_AddStringToObject(u, "email", user.email);
        cJSON_AddStringToObject(u, "role", user.role);
        cJSON_AddBoolToObject(u, "email_verified", user.email_verified);
        pgf_api_result_t r = { o, 201 };
        return r;
    }

    /* Secure default: identical response for OK and CONFLICT (no existence signal);
     * only a genuine server error differs, which is not correlated with existence. */
    if (rc == PGF_AUTH_OK || rc == PGF_AUTH_CONFLICT) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddStringToObject(o, "message", "registration received");
        pgf_api_result_t r = { o, 202 };
        return r;
    }
    return result_error(500, "server error");
}
