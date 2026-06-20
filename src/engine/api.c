#include "api.h"
#include "schema_catalog.h"
#include "query_builder.h"
#include "result_json.h"
#include "cel_apps.h"
#include "cel_hooks.h"
#include "cel_hook_state.h"
#include "cel_val.h"
#include "core/app_db.h"
#include "core/auth.h"
#include "core/mfa.h"
#include "core/oauth.h"
#include "core/mailer.h"
#include "core/base64url.h"
#include "core/metrics.h"
#include "logger.h"

#include <sqlite3.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The app every data query runs against is the process's current app
 * (app_db_current(), set at startup). Interim: one global app until request
 * routing resolves Host/path → bundle per request. */

#define SESSION_TTL_SECONDS (24 * 3600)
#define MIN_PASSWORD_LEN 8
/* Cap the password length: Argon2id processes the whole input, so a multi-MB
 * password is a CPU-DoS. 128 is well above any real password. */
#define MAX_PASSWORD_LEN 128

static cel_api_result_t session_result(const char *token, const cel_user_t *user);
static void send_email_verification(const char *user_id);
static cJSON *identity_to_json(const cel_identity_t *who);   /* defined near cel_api_rpc */

/* The hook-contract op name for a CRUD action. */
static const char *action_name(cel_action_t a) {
    switch (a) {
        case CEL_ACT_LIST:   return "list";
        case CEL_ACT_GET:    return "get";
        case CEL_ACT_CREATE: return "create";
        case CEL_ACT_UPDATE: return "update";
        case CEL_ACT_DELETE: return "delete";
    }
    return "?";
}

/* Run authorize() for a read (list table-level with row=NULL; get with the fetched
 * row), binding a hook db connection for the phase. Returns 1 if the app's hook
 * denies (caller -> 403); 0 if allowed or the app has no hooks. */
static int hook_denies_read(const cel_identity_t *who, const char *op,
                            const char *table, const cel_val_t *row) {
    cel_lua_t *hooks = cel_hook_app_state(cel_apps_current_hooks());
    if (!hooks) return 0;
    cJSON *who_v = identity_to_json(who);
    app_db_t *adb = app_db_current();
    sqlite3 *hc = adb ? app_db_conn_acquire(adb) : NULL;
    cel_hooks_set_db(hc);
    int denied = cel_hooks_authorize(hooks, op, table, row, (const cel_val_t *)who_v) == 0;
    cel_hooks_set_db(NULL);
    if (hc) app_db_conn_release(adb, hc);
    cJSON_Delete(who_v);
    return denied;
}

static cel_api_result_t result_error(int status, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "error");
    cJSON_AddStringToObject(o, "message", message);
    cel_api_result_t r = { o, status };
    return r;
}

/* Resolve a session token to the caller's identity (cache-aware). Lives here (not
 * in policy.c) because it needs the auth layer; declared in policy.h. */
void cel_identity_from_token(const char *token, cel_identity_t *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->role, sizeof out->role, "%s", "anon");
    if (!token || !*token) return;

    cel_user_t u;
    if (cel_auth_resolve(token, &u) == CEL_AUTH_OK) {   /* no DB hit on a cache hit */
        out->authenticated = true;
        snprintf(out->user_id, sizeof out->user_id, "%s", u.id);
        snprintf(out->role, sizeof out->role, "%s", u.role);
    }
}

static const cel_table_t *resolve_table(const cJSON *req) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(req, "table");
    if (!cJSON_IsString(t)) return NULL;
    return cel_catalog_find(cel_catalog_active(), t->valuestring);
}

/* Map a SQLite (extended) result code from a failed statement to an HTTP status. */
static int map_sqlite_err(int rc) {
    switch (rc) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY: return 409;
        case SQLITE_CONSTRAINT_FOREIGNKEY:
        case SQLITE_CONSTRAINT_NOTNULL:
        case SQLITE_CONSTRAINT_CHECK:
        case SQLITE_MISMATCH:              return 400;
        default:
            if ((rc & 0xFF) == SQLITE_CONSTRAINT) return 400;  /* any other constraint */
            return 500;
    }
}

/* Generic, client-safe message for a failed statement. NEVER return the raw
 * sqlite3_errmsg: it embeds table / column / constraint names and the offending
 * values (schema disclosure), and a unique-violation message is a row-existence
 * oracle. The full diagnostic is still logged server-side. (M-7) */
static const char *sqlite_err_message(int rc) {
    switch (rc) {
        case SQLITE_CONSTRAINT_UNIQUE:
        case SQLITE_CONSTRAINT_PRIMARYKEY: return "conflict";
        case SQLITE_CONSTRAINT_FOREIGNKEY: return "invalid reference";
        case SQLITE_CONSTRAINT_NOTNULL:    return "a required field is missing";
        case SQLITE_CONSTRAINT_CHECK:      return "a value failed a constraint check";
        case SQLITE_MISMATCH:              return "invalid input value";
        default:
            if ((rc & 0xFF) == SQLITE_CONSTRAINT) return "a value failed a constraint check";
            return "request failed";
    }
}

/* Execute a built query on an ALREADY-HELD connection `c` — no pool acquire, no
 * write lock. This is the inner executor: the read paths wrap it with run_rows
 * (acquire a connection), while the write path runs it inside an explicit
 * transaction (run_write) so before()'s hook writes, the main write, and the
 * RETURNING read are one atomic unit. On error sets *http + a client-safe errmsg.
 *
 * The serializer steps to completion; sqlite3_reset then surfaces any step
 * error (constraint / type mismatch / I/O) — checked so a failed write is
 * reported, not silently treated as an empty result. The statement comes from the
 * per-connection cache (parse+plan once per distinct SQL, not per request) and is
 * owned by it — we reset, never finalize, so it stays compiled for the next call. */
static cJSON *run_rows_on(sqlite3 *c, const cel_query_t *q, const cel_table_t *t,
                          int *http, char *errmsg, size_t errlen) {
    sqlite3_stmt *st = app_db_stmt_cached(app_db_current(), c, q->sql);
    if (!st) {
        LOG_ERROR("query prepare failed: %s | sql=%s", sqlite3_errmsg(c), q->sql);
        *http = 500; if (errmsg) snprintf(errmsg, errlen, "query failed");
        return NULL;
    }
    /* params are pushed in order with no reuse → bind params[i] to ?(i+1) */
    for (int i = 0; i < q->nparams; i++) {
        if (q->params[i]) sqlite3_bind_text(st, i + 1, q->params[i], -1, SQLITE_TRANSIENT);
        else              sqlite3_bind_null(st, i + 1);
    }

    /* t == NULL: a table-less result (e.g. count/aggregate) — typed by storage class. */
    cJSON *rows = t ? cel_stmt_rows_to_json(st, t) : cel_stmt_result_to_json(st);
    int rc = sqlite3_reset(st);   /* surfaces deferred step errors; keeps stmt cached */
    if (rc != SQLITE_OK) {
        *http = map_sqlite_err(rc);
        if (errmsg) snprintf(errmsg, errlen, "%s",
                             (*http < 500) ? sqlite_err_message(rc) : "query failed");
        LOG_ERROR("query failed [%d]: %s | sql=%s", rc, sqlite3_errmsg(c), q->sql);
        if (rows) { cJSON_Delete(rows); rows = NULL; }
    }
    return rows;
}

/* Run a built query, acquiring a connection (and, for a write, the per-app write
 * lock) for its duration. Used by the READ paths (list/get/embed/aggregate); the
 * write path runs inside an explicit transaction via run_rows_on. */
static cJSON *run_rows(const cel_query_t *q, const cel_table_t *t, int is_write,
                       int *http, char *errmsg, size_t errlen) {
    app_db_t *app = app_db_current();
    if (!app) { *http = 500; if (errmsg) snprintf(errmsg, errlen, "database unavailable"); return NULL; }

    if (is_write) app_db_write_lock(app);
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) {
        if (is_write) app_db_write_unlock(app);
        *http = 500; if (errmsg) snprintf(errmsg, errlen, "database unavailable");
        return NULL;
    }
    cJSON *rows = run_rows_on(c, q, t, http, errmsg, errlen);
    app_db_conn_release(app, c);
    if (is_write) app_db_write_unlock(app);
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

static int make_scope(const cel_table_t *t, cel_action_t action,
                      const cel_identity_t *who, cel_scope_t *scope) {
    memset(scope, 0, sizeof *scope);   /* every rule slot starts EQ/zeroed */
    scope->count = 0;

    /* Isolation between apps is the SQLite file boundary (no tenant_id, no RLS) —
     * so the only row-level scope here is per-row ownership within the app. */

    /* Owner scope (row-level ownership). May be a single
     * column (EQ), any of several (OR), or membership in a related table (VIA). */
    cel_owner_spec_t os;
    if (cel_policy_owner_scope(t->name, action, who->role, &os)) {
        /* SECURITY (H-9): OR (owner_any) / VIA (owner_via) are read filters — they
         * cannot be enforced on INSERT (no single column to force, no membership to
         * assert at create time). A create policy using them would leave ownership
         * client-controlled (owner spoofing) or the row unowned, so reject the
         * misconfiguration (-> 500) rather than silently create a spoofable row.
         * Only EQ (owner_column) is forceable on create. */
        if (action == CEL_ACT_CREATE && os.kind != CEL_OWNER_EQ) return -1;
        if (scope->count >= CEL_MAX_SCOPE) return -1;
        cel_scope_rule_t *r = &scope->rule[scope->count];
        r->value = who->user_id;
        switch (os.kind) {
        case CEL_OWNER_EQ:
            if (!cel_table_column(t, os.column)) return -1;
            r->kind = CEL_SCOPE_EQ; r->column = os.column;
            break;
        case CEL_OWNER_ANY:
            if (os.ncolumns < 1 || os.ncolumns > CEL_MAX_OR) return -1;
            for (int i = 0; i < os.ncolumns; i++) {
                if (!cel_table_column(t, os.columns[i])) return -1;
                r->cols[i] = os.columns[i];
            }
            r->kind = CEL_SCOPE_OR; r->ncols = os.ncolumns;
            break;
        case CEL_OWNER_VIA:
            /* local is a column on this table; the related identifiers can't be
             * catalog-checked here, so charset-validate them (then quoted). */
            if (!cel_table_column(t, os.via.local)) return -1;
            if (!is_safe_ident(os.via.table) || !is_safe_ident(os.via.ref) ||
                !is_safe_ident(os.via.user)) return -1;
            r->kind = CEL_SCOPE_VIA;
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
static void rt_emit(const char *table, cel_action_t action, const cJSON *row) {
    if (!cel_realtime_active()) return;
    if (!cel_policy_realtime_enabled(table)) return;
    const char *op = action == CEL_ACT_CREATE ? "INSERT"
                   : action == CEL_ACT_UPDATE ? "UPDATE"
                   : action == CEL_ACT_DELETE ? "DELETE" : "?";
    cel_realtime_publish(app_db_current(), table, op, row);   /* scope delivery to this app */
}

/* Shared shape for the write builders: build -> run -> {status, row}. */
typedef int (*build_fn)(const cel_table_t *, const cJSON *, const cel_scope_t *,
                        cel_query_t *, char *, size_t);

/* The body of a write, run INSIDE an open transaction on connection `c`: before()
 * + authorize() (their hook db bound to `c`, so cellar.exec writes enroll in this
 * txn), then build + execute, taking the RETURNING row. Returns 0 on success (and
 * sets *out_row, possibly NULL when require_row is 0), or an HTTP error code with
 * a client-safe message in `msg`. The caller commits on 0, rolls back otherwise. */
static int write_txn_body(sqlite3 *c, const cel_identity_t *who, const cJSON *req,
                          const cel_table_t *t, build_fn build, cel_action_t action,
                          int require_row, cel_lua_t *hooks, const cJSON *who_v,
                          const char *opn, cJSON **out_row, char *msg, size_t msglen) {
    *out_row = NULL;

    if (hooks) {
        cel_hooks_set_db(c);   /* before()'s cellar.query/exec run on the txn conn */
        cJSON *vals = cJSON_GetObjectItemCaseSensitive(req, "values");   /* mutable; NULL for delete */
        char herr[256] = {0};
        int rejected = cel_hooks_before(hooks, opn, t->name, (cel_val_t *)vals,
                                        (const cel_val_t *)who_v, herr, sizeof herr) != 0;
        int denied = !rejected && cel_hooks_authorize(hooks, opn, t->name,
                                                      (const cel_val_t *)vals, (const cel_val_t *)who_v) == 0;
        cel_hooks_set_db(NULL);
        if (rejected) { snprintf(msg, msglen, "%s", herr[0] ? herr : "rejected by hook"); return 400; }
        if (denied)   { snprintf(msg, msglen, "forbidden"); return 403; }
    }

    cel_scope_t scope;
    if (make_scope(t, action, who, &scope) != 0) { snprintf(msg, msglen, "policy misconfiguration"); return 500; }

    char err[256] = {0};
    cel_query_t q;
    if (build(t, req, &scope, &q, err, sizeof err) != 0) {
        snprintf(msg, msglen, "%s", err[0] ? err : "bad request"); return 400;
    }

    int http = 200;
    char emsg[256] = {0};
    cJSON *rows = run_rows_on(c, &q, t, &http, emsg, sizeof emsg);   /* on the txn conn */
    cel_query_free(&q);
    if (!rows) { snprintf(msg, msglen, "%s", emsg[0] ? emsg : "query failed"); return http; }

    int found = cJSON_GetArraySize(rows) > 0;
    *out_row = found ? cJSON_DetachItemFromArray(rows, 0) : NULL;
    cJSON_Delete(rows);
    if (require_row && !found) { snprintf(msg, msglen, "not found"); return 404; }
    return 0;
}

static cel_api_result_t run_write(const cel_identity_t *who, const cJSON *req,
                                  build_fn build, cel_action_t action,
                                  int ok_status, int require_row) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const cel_table_t *t = resolve_table(req);
    if (!t) return result_error(404, "unknown table");
    if (!cel_policy_allows(t->name, action, who->role)) return result_error(403, "forbidden");

    app_db_t *adb = app_db_current();
    if (!adb) return result_error(500, "database unavailable");

    cel_lua_t *hooks = cel_hook_app_state(cel_apps_current_hooks());
    cJSON *who_v = hooks ? identity_to_json(who) : NULL;
    const char *opn = action_name(action);

    /* One pinned connection + the per-app write lock for the whole request
     * transaction (design §8/§14): before()'s hook writes, the main write, and the
     * RETURNING read are atomic, and a hook's cellar.query/exec run on this same
     * connection — so they see, and roll back with, the in-flight changes. before()
     * and authorize() therefore run under the write lock (writers for this app
     * serialize on the slowest before()); after() stays post-commit. */
    app_db_write_lock(adb);
    sqlite3 *c = app_db_conn_acquire(adb);
    if (!c) { app_db_write_unlock(adb); cJSON_Delete(who_v); return result_error(500, "database unavailable"); }

    if (sqlite3_exec(c, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        app_db_conn_release(adb, c); app_db_write_unlock(adb); cJSON_Delete(who_v);
        return result_error(500, "could not begin transaction");
    }

    cJSON *row = NULL;
    char msg[256] = {0};
    int rc = write_txn_body(c, who, req, t, build, action, require_row, hooks, who_v, opn,
                            &row, msg, sizeof msg);

    if (rc != 0) {                                  /* reject / deny / build / query error → roll back */
        sqlite3_exec(c, "ROLLBACK", NULL, NULL, NULL);
        cel_hooks_set_db(NULL);
        app_db_conn_release(adb, c); app_db_write_unlock(adb);
        cJSON_Delete(row); cJSON_Delete(who_v);
        return result_error(rc, msg[0] ? msg : "request failed");
    }
    if (sqlite3_exec(c, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_ERROR("commit failed: %s", sqlite3_errmsg(c));
        sqlite3_exec(c, "ROLLBACK", NULL, NULL, NULL);
        app_db_conn_release(adb, c); app_db_write_unlock(adb);
        cJSON_Delete(row); cJSON_Delete(who_v);
        return result_error(500, "commit failed");
    }
    app_db_conn_release(adb, c);
    app_db_write_unlock(adb);

    if (row) rt_emit(t->name, action, row);   /* realtime fan-out, post-commit */

    /* after(): post-commit side effects on the committed row, on a separate pooled
     * connection (its writes are NOT part of the committed txn — §14). A fault is
     * logged, not fatal — the write already happened. */
    if (hooks && row) {
        sqlite3 *hc = app_db_conn_acquire(adb);
        cel_hooks_set_db(hc);
        cel_hooks_after(hooks, opn, t->name, (const cel_val_t *)row, (const cel_val_t *)who_v);
        cel_hooks_set_db(NULL);
        if (hc) app_db_conn_release(adb, hc);
    }
    cJSON_Delete(who_v);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    if (row) cJSON_AddItemToObject(o, "row", row);
    cel_api_result_t r = { o, ok_status };
    return r;
}

/* ---- relationship embedding (richer read) ---------------------------------- */

typedef struct {
    int   to_many;             /* 0 = to-one (forward FK); 1 = to-many (reverse FK) */
    const cel_table_t *remote; /* the related table to pull in */
    const char *local_key;     /* join column on the base table */
    const char *remote_key;    /* join column on the remote table */
} cel_relation_t;

/* Resolve an embed name (a RELATED TABLE NAME) against the catalog FK graph: a
 * forward FK on the base table (base.col -> name.pk) is to-one; a reverse FK
 * (name.col -> base.pk) is to-many. Ambiguity (more than one FK either way) is
 * rejected — never a silent guess. Returns an HTTP status (0 == resolved). */
static int resolve_relation(const cel_table_t *base, const char *name,
                            cel_relation_t *out, char *err, size_t errlen) {
    const cel_table_t *remote = cel_catalog_find(cel_catalog_active(), name);
    if (!remote) { snprintf(err, errlen, "no relation '%s' on '%s'", name, base->name); return 400; }

    const cel_column_t *fk = NULL; int nfk = 0;          /* to-one: forward FK on base */
    for (int i = 0; i < base->ncols; i++)
        if (base->cols[i].is_fk && !strcmp(base->cols[i].fk_table, name)) { fk = &base->cols[i]; nfk++; }
    if (nfk > 1) { snprintf(err, errlen, "ambiguous relation '%s' (multiple FKs)", name); return 400; }
    if (nfk == 1) {
        out->to_many = 0; out->remote = remote;
        out->local_key = fk->name; out->remote_key = fk->fk_column;
        return 0;
    }

    if (base->pk_index < 0) { snprintf(err, errlen, "no relation '%s' on '%s'", name, base->name); return 400; }
    const cel_column_t *rfk = NULL; int nrfk = 0;        /* to-many: reverse FK on remote */
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
static int embed_one(const cel_identity_t *who, const cel_table_t *base, cJSON *rows,
                     const char *name, const cel_table_t **out_remote, bool *out_to_many,
                     char *err, size_t errlen) {
    cel_relation_t rel;
    int rc = resolve_relation(base, name, &rel, err, errlen);
    if (rc) return rc;
    *out_remote = rel.remote;        /* so a dotted path can recurse into these rows */
    *out_to_many = rel.to_many;

    /* The related table is read on the caller's behalf — it must pass the same
     * read policy a direct LIST would. No leaking related rows you can't see. */
    if (!cel_policy_allows(rel.remote->name, CEL_ACT_LIST, who->role)) {
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
    cJSON_AddNumberToObject(sreq, "limit", CEL_LIST_MAX_LIMIT);

    cel_scope_t scope;
    if (make_scope(rel.remote, CEL_ACT_LIST, who, &scope) != 0) {
        cJSON_Delete(sreq); snprintf(err, errlen, "policy misconfiguration"); return 500;
    }
    cel_query_t q; char qerr[256] = {0};
    if (cel_build_list(rel.remote, sreq, &scope, NULL, &q, qerr, sizeof qerr) != 0) {
        cJSON_Delete(sreq); snprintf(err, errlen, "%s", qerr); return 400;
    }
    int http = 200; char emsg[256] = {0};
    cJSON *related = run_rows(&q, rel.remote, 0, &http, emsg, sizeof emsg);
    cel_query_free(&q);
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

#define CEL_MAX_EMBED_DEPTH 4

/* Embed a (possibly dotted) relation path like "order_items.product": embed the
 * first relation into `rows`, then recurse into the just-embedded rows for the
 * rest. Each level re-runs the caller's authz + row scope (embed_one). Returns an
 * HTTP status (0 == ok). */
static int embed_path(const cel_identity_t *who, const cel_table_t *base, cJSON *rows,
                      const char *path, int depth, char *err, size_t errlen) {
    if (depth > CEL_MAX_EMBED_DEPTH) { snprintf(err, errlen, "embed nested too deeply"); return 400; }

    char first[64];
    const char *dot = strchr(path, '.');
    size_t flen = dot ? (size_t)(dot - path) : strlen(path);
    if (flen == 0 || flen >= sizeof first) { snprintf(err, errlen, "bad embed path"); return 400; }
    memcpy(first, path, flen);
    first[flen] = '\0';
    const char *rest = dot ? dot + 1 : NULL;

    const cel_table_t *remote = NULL;
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
    if (cel_b64url_decode(token, strlen(token), buf, sizeof buf - 1, &n) != 0) { *bad = true; return NULL; }
    buf[n] = '\0';
    cJSON *arr = cJSON_Parse((char *)buf);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); *bad = true; return NULL; }
    return arr;
}

/* Build the next-page cursor token from the last row's sort-key values. */
static char *encode_next_cursor(const cel_table_t *t, const cJSON *req, const cJSON *last_row) {
    cel_sortkey_t keys[CEL_MAX_SORTKEYS];
    char e[128] = {0};
    int nk = cel_resolve_sortkeys(t, req, keys, CEL_MAX_SORTKEYS, e, sizeof e);
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
    if (tok && cel_b64url_encode((const unsigned char *)json, strlen(json), tok, cap) != 0) { free(tok); tok = NULL; }
    free(json);
    return tok;
}

cel_api_result_t cel_api_list(const cel_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const cel_table_t *t = resolve_table(req);
    if (!t) return result_error(404, "unknown table");
    if (!cel_policy_allows(t->name, CEL_ACT_LIST, who->role)) return result_error(403, "forbidden");
    /* authorize() is a table-level gate for list (no single row). */
    if (hook_denies_read(who, "list", t->name, NULL)) return result_error(403, "forbidden");

    cel_scope_t scope;
    if (make_scope(t, CEL_ACT_LIST, who, &scope) != 0) return result_error(500, "policy misconfiguration");

    /* Aggregate mode: { group?, aggregate? } -> GROUP BY rollups under the same
     * filters + row scope (so totals never include rows the caller can't see). A
     * different result shape, so it short-circuits the row/embed/cursor path. */
    const cJSON *grp = cJSON_GetObjectItemCaseSensitive(req, "group");
    const cJSON *agg = cJSON_GetObjectItemCaseSensitive(req, "aggregate");
    if ((cJSON_IsArray(grp) && cJSON_GetArraySize(grp) > 0) ||
        (cJSON_IsArray(agg) && cJSON_GetArraySize(agg) > 0)) {
        char aerr[256] = {0};
        cel_query_t aq;
        if (cel_build_aggregate(t, req, &scope, &aq, aerr, sizeof aerr) != 0)
            return result_error(400, aerr);
        int ahttp = 200; char aemsg[256] = {0};
        cJSON *arows = run_rows(&aq, NULL, 0, &ahttp, aemsg, sizeof aemsg);
        cel_query_free(&aq);
        if (!arows) return result_error(ahttp, aemsg[0] ? aemsg : "query failed");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(arows));
        cJSON_AddItemToObject(o, "rows", arows);
        cel_api_result_t r = { o, 200 };
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
    cel_query_t q;
    int brc = cel_build_list(t, req, &scope, cursor_vals, &q, err, sizeof err);
    cJSON_Delete(cursor_vals);   /* values are copied into the query params */
    if (brc != 0) return result_error(400, err);

    int http = 200;
    char emsg[256] = {0};
    cJSON *rows = run_rows(&q, t, 0, &http, emsg, sizeof emsg);
    cel_query_free(&q);
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
        cel_query_t cq; char cerr[256] = {0};
        if (cel_build_count(t, req, &scope, &cq, cerr, sizeof cerr) == 0) {
            int chttp = 200; char cemsg[256] = {0};
            cJSON *cr = run_rows(&cq, NULL, 0, &chttp, cemsg, sizeof cemsg);
            cel_query_free(&cq);
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
        long limit = CEL_LIST_DEFAULT_LIMIT;
        const cJSON *jl = cJSON_GetObjectItemCaseSensitive(req, "limit");
        if (cJSON_IsNumber(jl)) {
            limit = (long)jl->valuedouble;
            if (limit < 1) limit = 1;
            if (limit > CEL_LIST_MAX_LIMIT) limit = CEL_LIST_MAX_LIMIT;
        }
        if (page >= limit) {
            char *nc = encode_next_cursor(t, req, cJSON_GetArrayItem(rows, page - 1));
            if (nc) { cJSON_AddStringToObject(o, "next_cursor", nc); free(nc); }
        }
    }

    cJSON_AddItemToObject(o, "rows", rows);
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_get(const cel_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const cel_table_t *t = resolve_table(req);
    if (!t) return result_error(404, "unknown table");
    if (!cel_policy_allows(t->name, CEL_ACT_GET, who->role)) return result_error(403, "forbidden");

    cel_scope_t scope;
    if (make_scope(t, CEL_ACT_GET, who, &scope) != 0) return result_error(500, "policy misconfiguration");

    char err[256] = {0};
    cel_query_t q;
    if (cel_build_get(t, req, &scope, &q, err, sizeof err) != 0) return result_error(400, err);

    int http = 200;
    char emsg[256] = {0};
    cJSON *rows = run_rows(&q, t, 0, &http, emsg, sizeof emsg);
    cel_query_free(&q);
    if (!rows) return result_error(http, emsg[0] ? emsg : "query failed");

    if (cJSON_GetArraySize(rows) == 0) { cJSON_Delete(rows); return result_error(404, "not found"); }
    cJSON *row = cJSON_DetachItemFromArray(rows, 0);
    cJSON_Delete(rows);

    /* authorize() sees the fetched row (row-level read authz). */
    if (hook_denies_read(who, "get", t->name, (const cel_val_t *)row)) {
        cJSON_Delete(row); return result_error(403, "forbidden");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddItemToObject(o, "row", row);
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_create(const cel_identity_t *who, const cJSON *req) {
    return run_write(who, req, cel_build_create, CEL_ACT_CREATE, 201, 0);
}
cel_api_result_t cel_api_update(const cel_identity_t *who, const cJSON *req) {
    return run_write(who, req, cel_build_update, CEL_ACT_UPDATE, 200, 1);
}
cel_api_result_t cel_api_delete(const cel_identity_t *who, const cJSON *req) {
    return run_write(who, req, cel_build_delete, CEL_ACT_DELETE, 200, 1);
}

/* Build the `who` object a hook sees from the resolved identity. */
static cJSON *identity_to_json(const cel_identity_t *who) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "authenticated", who->authenticated);
    cJSON_AddStringToObject(o, "user_id", who->user_id);
    cJSON_AddStringToObject(o, "role", who->role);
    return o;
}

/* RPC is a Lua hook (design §8): rpc(name, args, who) -> result. The app's
 * hooks.lua is the authority — it inspects `who` and decides what to run. */
cel_api_result_t cel_api_rpc(const cel_identity_t *who, const cJSON *req) {
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(req, "fn");
    if (!cJSON_IsString(fn) || !fn->valuestring[0]) return result_error(400, "fn required");

    cel_lua_t *L = cel_hook_app_state(cel_apps_current_hooks());
    if (!L) return result_error(501, "rpc is not available (no hooks.lua for this app)");

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "args");   /* may be NULL */
    cJSON *who_v = identity_to_json(who);

    /* Bind a connection for the hook's cellar.query/exec. No request transaction
     * yet — writes autocommit per statement (atomic before/after lands in Step 3). */
    app_db_t *app = app_db_current();
    sqlite3 *conn = app ? app_db_conn_acquire(app) : NULL;
    cel_hooks_set_db(conn);

    char err[256];
    cel_val_t *res = cel_hooks_rpc(L, fn->valuestring,
                                   (const cel_val_t *)args, (const cel_val_t *)who_v,
                                   err, sizeof err);

    cel_hooks_set_db(NULL);
    if (conn) app_db_conn_release(app, conn);
    cJSON_Delete(who_v);

    if (!res) return result_error(400, err[0] ? err : "rpc failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddItemToObject(o, "result", (cJSON *)res);   /* transfers ownership */
    cel_api_result_t r = { o, 200 };
    return r;
}

void cel_rpc_audit_security_definer(void) {
    /* No-op on SQLite: there is no SECURITY DEFINER concept (that was a Postgres
     * RLS-bypass audit). RPC and its authorization move to the hook layer in
     * Phase 2. Kept as a symbol for the existing startup call site. */
}

cel_api_result_t cel_api_schema(const cel_identity_t *who) {
    if (!who->authenticated) return result_error(401, "authentication required");
    cJSON *cat = cel_catalog_to_cjson(cel_catalog_active());
    cJSON_AddStringToObject(cat, "status", "ok");
    cel_api_result_t r = { cat, 200 };
    return r;
}

cel_api_result_t cel_api_login(const cJSON *req) {
    const cJSON *email = cJSON_GetObjectItemCaseSensitive(req, "email");
    const cJSON *pass  = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(email) || !cJSON_IsString(pass))
        return result_error(400, "email and password required");
    /* Reject over-length input before it reaches Argon2id (CPU-DoS guard). */
    if (strlen(pass->valuestring) > MAX_PASSWORD_LEN)
        return result_error(401, "invalid credentials");

    char token[129], challenge[129];
    cel_user_t user;
    int rc = cel_auth_login(email->valuestring, pass->valuestring,
                            SESSION_TTL_SECONDS, token, sizeof token,
                            challenge, sizeof challenge, &user);
    if (rc == CEL_AUTH_MFA_REQUIRED) {
        /* Factor one passed; no session yet. The client submits the challenge +
         * a TOTP code to /auth/mfa/verify to finish logging in. */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "mfa_required");
        cJSON_AddStringToObject(o, "challenge", challenge);
        cel_api_result_t r = { o, 200 };
        return r;
    }
    /* L-1: a locked account must be indistinguishable from a wrong password or a
     * non-existent user — a 429 "account locked" (vs 401) confirms the email is
     * registered and under attack, and leaks the locked->unlocked transition. Both
     * return a uniform 401. (Lockout still throttles server-side; the account just
     * isn't advertised as locked. Trade-off: a locked legitimate user sees
     * "invalid credentials" rather than a lockout notice.) */
    if (rc == CEL_AUTH_LOCKED || rc == CEL_AUTH_INVALID)
        return result_error(401, "invalid credentials");
    if (rc != CEL_AUTH_OK)      return result_error(500, "server error");

    return session_result(token, &user);
}

/* Shared {status, token, user} body for a completed login (password path or the
 * MFA second step). */
static cel_api_result_t session_result(const char *token, const cel_user_t *user) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "token", token);
    cJSON *u = cJSON_AddObjectToObject(o, "user");
    cJSON_AddStringToObject(u, "id", user->id);
    cJSON_AddStringToObject(u, "email", user->email);
    cJSON_AddStringToObject(u, "role", user->role);
    cJSON_AddBoolToObject(u, "email_verified", user->email_verified);
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_mfa_enroll(const cel_identity_t *who) {
    if (!who->authenticated) return result_error(401, "authentication required");
    char secret[64], uri[256];
    int rc = cel_mfa_enroll(who->user_id, secret, sizeof secret, uri, sizeof uri);
    if (rc == CEL_MFA_DISABLED) return result_error(403, "two-factor auth is not enabled");
    if (rc == CEL_MFA_ALREADY)  return result_error(409, "already enrolled");
    if (rc != CEL_MFA_OK)       return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "secret", secret);          /* show once, for the QR */
    cJSON_AddStringToObject(o, "otpauth_uri", uri);
    cel_api_result_t r = { o, 200 };
    return r;
}

/* The 6-digit code field, shared by confirm/disable. */
static const char *mfa_code(const cJSON *req) {
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(req, "code");
    return cJSON_IsString(code) ? code->valuestring : NULL;
}

/* The one-time recovery codes as a JSON array (returned once at confirm/regen). */
static cJSON *recovery_codes_json(char codes[][CEL_MFA_RECOVERY_LEN]) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < CEL_MFA_RECOVERY_N; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(codes[i]));
    return arr;
}

cel_api_result_t cel_api_mfa_confirm(const cel_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const char *code = mfa_code(req);
    if (!code) return result_error(400, "code required");
    char codes[CEL_MFA_RECOVERY_N][CEL_MFA_RECOVERY_LEN];
    int rc = cel_mfa_confirm(who->user_id, code, codes);
    if (rc == CEL_MFA_DISABLED)     return result_error(403, "two-factor auth is not enabled");
    if (rc == CEL_MFA_NOT_ENROLLED) return result_error(400, "no pending enrollment");
    if (rc == CEL_MFA_INVALID)      return result_error(401, "invalid code");
    if (rc != CEL_MFA_OK)           return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "confirmed");
    cJSON_AddItemToObject(o, "recovery_codes", recovery_codes_json(codes));  /* show once */
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_mfa_recovery(const cel_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const char *code = mfa_code(req);
    if (!code) return result_error(400, "code required");
    char codes[CEL_MFA_RECOVERY_N][CEL_MFA_RECOVERY_LEN];
    int rc = cel_mfa_regenerate_recovery(who->user_id, code, codes);
    if (rc == CEL_MFA_NOT_ENROLLED) return result_error(400, "not enrolled");
    if (rc == CEL_MFA_INVALID)      return result_error(401, "invalid code");
    if (rc != CEL_MFA_OK)           return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddItemToObject(o, "recovery_codes", recovery_codes_json(codes));
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_mfa_disable(const cel_identity_t *who, const cJSON *req) {
    if (!who->authenticated) return result_error(401, "authentication required");
    const char *code = mfa_code(req);
    if (!code) return result_error(400, "code required");
    int rc = cel_mfa_disable(who->user_id, code);
    if (rc == CEL_MFA_NOT_ENROLLED) return result_error(400, "not enrolled");
    if (rc == CEL_MFA_INVALID)      return result_error(401, "invalid code");
    if (rc != CEL_MFA_OK)           return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "disabled");
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_mfa_verify(const cJSON *req) {
    const cJSON *challenge = cJSON_GetObjectItemCaseSensitive(req, "challenge");
    const cJSON *code      = cJSON_GetObjectItemCaseSensitive(req, "code");
    if (!cJSON_IsString(challenge) || !cJSON_IsString(code))
        return result_error(400, "challenge and code required");

    char token[129];
    cel_user_t user;
    int rc = cel_mfa_verify_login(challenge->valuestring, code->valuestring,
                                  SESSION_TTL_SECONDS, token, sizeof token, &user);
    if (rc == CEL_MFA_INVALID) return result_error(401, "invalid code or challenge");
    if (rc != CEL_MFA_OK)      return result_error(500, "server error");

    return session_result(token, &user);
}

/* Build + send the email-verification message for `user_id` (best-effort; no-op
 * if the mailer is off or the email is already verified). Shared by register and
 * the resend endpoint. */
static void send_email_verification(const char *user_id) {
    if (!cel_mail_enabled()) return;
    char token[129], to[256];
    if (cel_auth_create_email_verification(user_id, token, sizeof token, to, sizeof to) != CEL_AUTH_OK)
        return;
    const char *app = getenv("CEL_APP_URL");
    char body[1024];
    if (app && *app)
        snprintf(body, sizeof body,
            "Welcome! Please confirm your email address:\r\n%s/verify-email?token=%s\r\n\r\n"
            "This link expires in 24 hours.\r\n", app, token);
    else
        snprintf(body, sizeof body,
            "Welcome! Confirm your email address with this token (expires in 24 hours):\r\n%s\r\n",
            token);
    cel_mail_send(to, "Verify your email", body);
}

cel_api_result_t cel_api_verify_email(const cJSON *req) {
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(req, "token");
    if (!cJSON_IsString(token)) return result_error(400, "token required");
    int rc = cel_auth_verify_email(token->valuestring);
    if (rc == CEL_AUTH_INVALID) return result_error(400, "invalid or expired token");
    if (rc != CEL_AUTH_OK)      return result_error(500, "server error");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "email verified");
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_verify_email_resend(const cel_identity_t *who) {
    if (!who->authenticated) return result_error(401, "authentication required");
    send_email_verification(who->user_id);   /* best-effort; idempotent if already verified */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "if your email is unverified, a verification link has been sent");
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_password_forgot(const cJSON *req) {
    const cJSON *email = cJSON_GetObjectItemCaseSensitive(req, "email");
    if (!cJSON_IsString(email) || !email->valuestring[0])
        return result_error(400, "email required");

    char token[129];
    if (cel_auth_create_password_reset(email->valuestring, token, sizeof token) == CEL_AUTH_OK
        && cel_mail_enabled()) {
        const char *app = getenv("CEL_APP_URL");
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
        cel_mail_send(email->valuestring, "Reset your password", body);
    }

    /* Always 200 with the same body — never reveal whether the email is registered. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "if that email is registered, a reset link has been sent");
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_password_reset(const cJSON *req) {
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(req, "token");
    const cJSON *pass  = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(token) || !cJSON_IsString(pass))
        return result_error(400, "token and password required");
    size_t plen = strlen(pass->valuestring);
    if (plen < MIN_PASSWORD_LEN) return result_error(400, "password too short (min 8 characters)");
    if (plen > MAX_PASSWORD_LEN) return result_error(400, "password too long (max 128 characters)");

    int rc = cel_auth_perform_password_reset(token->valuestring, pass->valuestring);
    if (rc == CEL_AUTH_INVALID) return result_error(400, "invalid or expired reset token");
    if (rc != CEL_AUTH_OK)      return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON_AddStringToObject(o, "message", "password updated");
    cel_api_result_t r = { o, 200 };
    return r;
}

cel_api_result_t cel_api_oauth(const cJSON *req) {
    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(req, "provider");
    const cJSON *token    = cJSON_GetObjectItemCaseSensitive(req, "id_token");
    if (!cJSON_IsString(provider) || !cJSON_IsString(token))
        return result_error(400, "provider and id_token required");

    /* Verify the ID token (signature + iss/aud/azp/exp/iat, optional nonce) before
     * trusting any claim. A client doing the nonce dance passes the nonce it minted. */
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(req, "nonce");
    const char *exp_nonce = cJSON_IsString(nonce) ? nonce->valuestring : NULL;
    cel_oauth_claims_t claims;
    char verr[128] = {0};
    if (cel_oauth_verify(provider->valuestring, token->valuestring, exp_nonce, &claims, verr, sizeof verr) != 0) {
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
    const char *prole = cel_role_default_signup(role, sizeof role) ? role : NULL;

    /* May this provider auto-link to an existing local account with this email?
     * Only if the operator trusts it for the email's domain (H-3). */
    bool link_trusted = cel_oauth_email_link_allowed(provider->valuestring, claims.email);

    char stoken[129], challenge[129];
    cel_user_t user;
    int rc = cel_auth_oauth_login(provider->valuestring, claims.sub, claims.email,
                                  claims.email_verified, link_trusted, prole, SESSION_TTL_SECONDS,
                                  stoken, sizeof stoken, challenge, sizeof challenge, &user);
    if (rc == CEL_AUTH_MFA_REQUIRED) {
        /* Federated factor one passed; no session yet. Same flow as password login:
         * the client submits the challenge + a TOTP code to /auth/mfa/verify. */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "mfa_required");
        cJSON_AddStringToObject(o, "challenge", challenge);
        cel_api_result_t r = { o, 200 };
        return r;
    }
    if (rc == CEL_AUTH_INVALID) return result_error(403, "no account for this identity");
    if (rc != CEL_AUTH_OK)      return result_error(500, "server error");
    return session_result(stoken, &user);
}

cel_api_result_t cel_api_create_user(const cel_identity_t *who, const cJSON *req) {
    if (!who->authenticated)               return result_error(401, "authentication required");
    if (!cel_role_is_superuser(who->role)) return result_error(403, "forbidden");

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
     * platform tier unreachable from in-band actors. */
    if (!strcmp(role, "platform_admin"))
        return result_error(403, "platform_admin can only be created out-of-band");

    char id[37];
    int rc = cel_auth_create_user(email->valuestring, pass->valuestring, role, id, sizeof id);
    if (rc == CEL_AUTH_CONFLICT) return result_error(409, "email already registered");
    if (rc != CEL_AUTH_OK)       return result_error(500, "server error");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "ok");
    cJSON *u = cJSON_AddObjectToObject(o, "user");
    cJSON_AddStringToObject(u, "id", id);
    cJSON_AddStringToObject(u, "email", email->valuestring);
    cJSON_AddStringToObject(u, "role", role);
    cel_api_result_t r = { o, 201 };
    return r;
}

/* Is `user_id` a participant for `key_val` via the membership table? One query
 * against the app's SQLite db: EXISTS(SELECT 1 FROM via_table WHERE via_ref = key
 * AND via_user = caller). The identifiers were charset-validated by make_scope, so
 * they are safe to quote; the values are bound. */
static bool rt_membership(const char *via_table, const char *via_ref, const char *via_user,
                          const char *key_val, const char *user_id) {
    app_db_t *app = app_db_current();
    if (!app) return false;
    sqlite3 *c = app_db_conn_acquire(app);
    if (!c) return false;
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT 1 FROM \"%s\" WHERE \"%s\" = ?1 AND \"%s\" = ?2 LIMIT 1",
             via_table, via_ref, via_user);
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(c, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key_val, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, user_id, -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(st) == SQLITE_ROW);
    }
    sqlite3_finalize(st);
    app_db_conn_release(app, c);
    return ok;
}

/* Publish-time re-authorization callback (M-5): the subscription carries the VIA
 * membership coordinates captured at subscribe; re-verify them against the live
 * membership table. */
bool cel_api_rt_recheck_member(const cel_subscription_t *sub) {
    if (!sub->via) return true;   /* not a membership subscription */
    return rt_membership(sub->via_table, sub->via_ref, sub->via_user,
                         sub->via_key, sub->user_id);
}

/* The on_realtime() delivery filter (registered with cel_realtime_set_filter).
 * Runs on the publishing app's worker thread, which has the app bound, so it
 * reaches that app's hooks. on_realtime is a PURE filter: no db is bound (a
 * cellar.query inside it errors → the dispatch fails closed → drop). */
bool cel_api_rt_filter(const cel_subscription_t *sub, const char *table,
                       const char *op, const cJSON *row) {
    cel_lua_t *hooks = cel_hook_app_state(cel_apps_current_hooks());
    if (!hooks) return true;   /* no hooks → deliver */

    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "table", table);
    cJSON_AddStringToObject(change, "op", op);
    cJSON_AddItemReferenceToObject(change, "row", (cJSON *)row);   /* row not owned */
    cJSON *subscriber = cJSON_CreateObject();
    cJSON_AddStringToObject(subscriber, "user_id", sub->user_id);
    cJSON_AddStringToObject(subscriber, "role", sub->role);

    int deliver = cel_hooks_on_realtime(hooks, (const cel_val_t *)change,
                                        (const cel_val_t *)subscriber);
    cJSON_Delete(change);
    cJSON_Delete(subscriber);
    return deliver != 0;
}

int cel_api_authorize_subscription(const cel_identity_t *who, const cJSON *req,
                                   cel_subscription_t *sub, char *errbuf, size_t errlen) {
    memset(sub, 0, sizeof *sub);
    sub->app = app_db_current();   /* the app this subscription belongs to (bound by the handler) */
    if (!who->authenticated) { snprintf(errbuf, errlen, "authentication required"); return 401; }
    const cel_table_t *t = resolve_table(req);
    if (!t) { snprintf(errbuf, errlen, "unknown table"); return 404; }
    if (!cel_policy_allows(t->name, CEL_ACT_LIST, who->role)) {
        snprintf(errbuf, errlen, "forbidden"); return 403;
    }
    if (!cel_policy_realtime_enabled(t->name)) {
        snprintf(errbuf, errlen, "realtime not enabled for table"); return 403;
    }
    snprintf(sub->table, sizeof sub->table, "%s", t->name);
    /* the subscriber identity the on_realtime hook sees (and VIA re-check uses) */
    snprintf(sub->user_id, sizeof sub->user_id, "%s", who->user_id);
    snprintf(sub->role,    sizeof sub->role,    "%s", who->role);

    cel_scope_t scope;
    if (make_scope(t, CEL_ACT_LIST, who, &scope) != 0) {
        snprintf(errbuf, errlen, "policy misconfiguration"); return 500;
    }

    /* optional { key: { column, value } } — required for a VIA (membership) rule */
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(req, "key");
    const cJSON *kc = cJSON_IsObject(key) ? cJSON_GetObjectItemCaseSensitive(key, "column") : NULL;
    const cJSON *kv = cJSON_IsObject(key) ? cJSON_GetObjectItemCaseSensitive(key, "value") : NULL;
    const char *key_col = cJSON_IsString(kc) ? kc->valuestring : NULL;
    const char *key_val = cJSON_IsString(kv) ? kv->valuestring : NULL;

    for (int i = 0; i < scope.count && sub->npreds < CEL_RT_MAX_PREDS; i++) {
        const cel_scope_rule_t *r = &scope.rule[i];
        cel_rt_pred_t *p = &sub->preds[sub->npreds];
        if (r->kind == CEL_SCOPE_EQ) {
            p->is_or = false;
            snprintf(p->column, sizeof p->column, "%s", r->column);
            snprintf(p->value,  sizeof p->value,  "%s", r->value);
            sub->npreds++;
        } else if (r->kind == CEL_SCOPE_OR) {
            p->is_or = true; p->ncols = r->ncols;
            for (int j = 0; j < r->ncols; j++)
                snprintf(p->cols[j], sizeof p->cols[j], "%s", r->cols[j]);
            snprintf(p->value, sizeof p->value, "%s", r->value);
            sub->npreds++;
        } else { /* CEL_SCOPE_VIA: require a key on the local column + membership */
            if (!key_col || !key_val || strcmp(key_col, r->via_local) != 0) {
                snprintf(errbuf, errlen,
                         "subscription requires key { column: \"%s\", value }", r->via_local);
                return 400;
            }
            if (!rt_membership(r->via_table, r->via_ref, r->via_user, key_val, who->user_id)) {
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
        }
    }
    return 200;
}

cel_api_result_t cel_api_register(const cJSON *req) {
    const cJSON *email  = cJSON_GetObjectItemCaseSensitive(req, "email");
    const cJSON *pass   = cJSON_GetObjectItemCaseSensitive(req, "password");
    const cJSON *role_j = cJSON_GetObjectItemCaseSensitive(req, "role");
    if (!cJSON_IsString(email) || !cJSON_IsString(pass) || !email->valuestring[0])
        return result_error(400, "email and password required");
    if (strlen(pass->valuestring) < MIN_PASSWORD_LEN)
        return result_error(400, "password too short (min 8 characters)");
    if (strlen(pass->valuestring) > MAX_PASSWORD_LEN)
        return result_error(400, "password too long (max 128 characters)");

    /* Resolve the requested role (explicit, else the configured default), then
     * enforce the signup whitelist — deny-by-default, superusers never allowed. */
    char role[32];
    if (cJSON_IsString(role_j) && role_j->valuestring[0]) {
        snprintf(role, sizeof role, "%s", role_j->valuestring);
    } else if (!cel_role_default_signup(role, sizeof role)) {
        return result_error(403, "self-registration is not enabled");
    }
    if (!cel_role_can_self_register(role))
        return result_error(403, "role is not open to self-registration");

    char token[129];
    cel_user_t user;
    int rc = cel_auth_register(email->valuestring, pass->valuestring, role,
                               SESSION_TTL_SECONDS, token, sizeof token, &user);

    if (rc == CEL_AUTH_OK) send_email_verification(user.id);  /* only on a real new account */

    /* H-5: by default register does NOT auto-login and does NOT reveal whether the
     * email already exists — a token (or a 409) returned only for NEW emails is an
     * account-enumeration oracle. A successful new registration and an existing-
     * email conflict return the SAME uniform 202; the client signs in separately
     * via /auth/login. Operators who accept the enumeration trade-off can restore
     * auto-login (201 + token, 409 on conflict) with CEL_REGISTER_AUTOLOGIN=1.
     * (Residual: when SMTP is configured the verification email above adds latency
     * on the new-account path only — a weaker timing side-channel, not closed here.) */
    const char *al = getenv("CEL_REGISTER_AUTOLOGIN");
    bool autologin = al && (!strcmp(al, "1") || !strcmp(al, "true") || !strcmp(al, "yes"));

    if (autologin) {
        if (rc == CEL_AUTH_CONFLICT) return result_error(409, "email already registered");
        if (rc != CEL_AUTH_OK)       return result_error(500, "server error");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddStringToObject(o, "token", token);
        cJSON *u = cJSON_AddObjectToObject(o, "user");
        cJSON_AddStringToObject(u, "id", user.id);
        cJSON_AddStringToObject(u, "email", user.email);
        cJSON_AddStringToObject(u, "role", user.role);
        cJSON_AddBoolToObject(u, "email_verified", user.email_verified);
        cel_api_result_t r = { o, 201 };
        return r;
    }

    /* Secure default: identical response for OK and CONFLICT (no existence signal);
     * only a genuine server error differs, which is not correlated with existence. */
    if (rc == CEL_AUTH_OK || rc == CEL_AUTH_CONFLICT) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddStringToObject(o, "message", "registration received");
        cel_api_result_t r = { o, 202 };
        return r;
    }
    return result_error(500, "server error");
}
