#include "migrate.h"
#include "db_connection.h"
#include "logger.h"

#include <libpq-fe.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed key for the migration advisory lock (serializes concurrent migrators). */
#define MIGRATE_LOCK_KEY "4242000001"

/* FNV-1a 64-bit of the SQL text -> 16 hex chars. Edit-detection, not security. */
static void checksum_hex(const char *s, char out[17]) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

static int run_sql(PGconn *c, const char *sql) {
    PGresult *r = PQexec(c, sql);   /* a migration may hold multiple statements */
    ExecStatusType st = PQresultStatus(r);
    int ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    if (!ok) LOG_ERROR("migrate: %s", PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

static int ensure_table(PGconn *c) {
    run_sql(c, "SET client_min_messages = warning");   /* quiet IF-NOT-EXISTS notices */
    if (run_sql(c,
        "CREATE TABLE IF NOT EXISTS pgf_migrations ("
        "  name text PRIMARY KEY,"
        "  checksum text NOT NULL DEFAULT '',"
        "  applied_at timestamptz NOT NULL DEFAULT now())") != 0) return -1;
    /* upgrade path for installs created before the checksum column existed */
    return run_sql(c, "ALTER TABLE pgf_migrations ADD COLUMN IF NOT EXISTS "
                      "checksum text NOT NULL DEFAULT ''");
}

/* Apply each pending migration in `list`. Already-applied ones are skipped, but
 * their checksum is verified (and backfilled for pre-checksum installs); an
 * edited-after-apply migration is a hard error. Returns 0/-1. */
static int apply_list(PGconn *c, const pgf_migration_t *list, int count,
                      const char *prefix, int *applied) {
    for (int i = 0; i < count; i++) {
        char name[256];
        snprintf(name, sizeof name, "%s%s", prefix, list[i].name);
        char sum[17];
        checksum_hex(list[i].sql, sum);

        const char *p1[1] = { name };
        PGresult *r = PQexecParams(c, "SELECT checksum FROM pgf_migrations WHERE name=$1",
                                   1, NULL, p1, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -1; }
        if (PQntuples(r) == 1) {
            const char *stored = PQgetvalue(r, 0, 0);
            if (stored[0] == '\0') {
                const char *up[2] = { sum, name };   /* backfill */
                PQclear(PQexecParams(c, "UPDATE pgf_migrations SET checksum=$1 WHERE name=$2",
                                     2, NULL, up, NULL, NULL, 0));
            } else if (strcmp(stored, sum) != 0) {
                LOG_ERROR("migrate: '%s' was modified after being applied "
                          "(checksum %s != %s) — migrations are immutable", name, stored, sum);
                PQclear(r);
                return -1;
            }
            PQclear(r);
            continue;
        }
        PQclear(r);

        LOG_INFO("migrate: applying %s", name);
        if (run_sql(c, "BEGIN") != 0) return -1;
        if (run_sql(c, list[i].sql) != 0) { run_sql(c, "ROLLBACK"); return -1; }
        const char *ins[2] = { name, sum };
        PGresult *r2 = PQexecParams(c, "INSERT INTO pgf_migrations(name, checksum) VALUES($1, $2)",
                                    2, NULL, ins, NULL, NULL, 0);
        int ok = PQresultStatus(r2) == PGRES_COMMAND_OK;
        PQclear(r2);
        if (!ok) { LOG_ERROR("migrate: could not record %s", name); run_sql(c, "ROLLBACK"); return -1; }
        if (run_sql(c, "COMMIT") != 0) return -1;
        if (applied) (*applied)++;
    }
    return 0;
}

int pgf_migrate_run(int with_demo, int with_tenancy, int *applied_out) {
    if (applied_out) *applied_out = 0;
    PGconn *c = db_connection_acquire();
    if (!c) { LOG_ERROR("migrate: no database connection"); return -1; }

    int rc = -1;
    if (run_sql(c, "SELECT pg_advisory_lock(" MIGRATE_LOCK_KEY ")") != 0) goto out;
    if (ensure_table(c) != 0) goto unlock;
    if (apply_list(c, PGF_MIGRATIONS, PGF_MIGRATIONS_COUNT, "", applied_out) != 0) goto unlock;
    if (with_tenancy &&
        apply_list(c, PGF_TENANCY_MIGRATIONS, PGF_TENANCY_MIGRATIONS_COUNT, "tenancy:", applied_out) != 0)
        goto unlock;
    if (with_demo &&
        apply_list(c, PGF_DEMO_MIGRATIONS, PGF_DEMO_MIGRATIONS_COUNT, "demo:", applied_out) != 0)
        goto unlock;
    rc = 0;
unlock:
    run_sql(c, "SELECT pg_advisory_unlock(" MIGRATE_LOCK_KEY ")");
out:
    db_connection_release(c);
    return rc;
}

static int status_list(PGconn *c, const pgf_migration_t *list, int count,
                       const char *prefix, int *done, int *pending) {
    for (int i = 0; i < count; i++) {
        char name[256];
        snprintf(name, sizeof name, "%s%s", prefix, list[i].name);
        const char *p[1] = { name };
        PGresult *r = PQexecParams(c, "SELECT 1 FROM pgf_migrations WHERE name=$1",
                                   1, NULL, p, NULL, NULL, 0);
        if (PQresultStatus(r) != PGRES_TUPLES_OK) { PQclear(r); return -1; }
        int applied = PQntuples(r) > 0;
        PQclear(r);
        LOG_INFO("  [%s] %s", applied ? "applied" : "pending", name);
        if (applied) (*done)++; else (*pending)++;
    }
    return 0;
}

/* A plain SQL identifier (letter/underscore then alnum/underscore, < 64 chars). */
static int is_safe_ident(const char *s) {
    if (!s || !*s || strlen(s) >= 64) return 0;
    int c0 = (unsigned char)s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return 0;
    for (const char *p = s; *p; p++) {
        int c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

int pgf_tenancy_protect(const char *tenant_column) {
    if (!is_safe_ident(tenant_column)) {
        LOG_ERROR("tenancy-protect: invalid tenant column '%s'", tenant_column ? tenant_column : "(null)");
        return -1;
    }
    PGconn *c = db_connection_acquire();
    if (!c) { LOG_ERROR("tenancy-protect: no database connection"); return -1; }

    /* Enable RLS + FORCE + the isolation policy on every public BASE TABLE that
     * carries the tenant column (excluding pgforge's own pgf_* tables). The
     * policy is fail-closed: an unset app.tenant_id (current_setting -> NULL)
     * matches no rows; the platform admin binds the '*' sentinel to see all.
     * FORCE makes the policy apply even to the table owner. (The serving role
     * must be NON-superuser for RLS to bite — superusers always bypass it.)
     * The whole thing is one DO block; the tenant column (validated above) is a
     * PL/pgSQL variable, and table names go through format(%I). */
    static const char *PRE =
        "SET client_min_messages = warning;\n"
        "DO $pgf$\nDECLARE r record; col text := '";
    static const char *SUF =
        "';\nBEGIN\n"
        "  FOR r IN SELECT c.table_name AS t FROM information_schema.columns c\n"
        "           JOIN information_schema.tables tb USING (table_schema, table_name)\n"
        "           WHERE c.table_schema='public' AND c.column_name=col\n"
        "             AND tb.table_type='BASE TABLE' AND left(c.table_name,4) <> 'pgf_' LOOP\n"
        "    EXECUTE format('ALTER TABLE public.%I ENABLE ROW LEVEL SECURITY', r.t);\n"
        "    EXECUTE format('ALTER TABLE public.%I FORCE ROW LEVEL SECURITY', r.t);\n"
        "    EXECUTE format('DROP POLICY IF EXISTS pgf_tenant_isolation ON public.%I', r.t);\n"
        "    EXECUTE format('CREATE POLICY pgf_tenant_isolation ON public.%I USING ("
                  "current_setting(''app.tenant_id'',true) = ''*'' OR "
                  "%I::text = current_setting(''app.tenant_id'',true))', r.t, col);\n"
        "    RAISE NOTICE 'pgforge: RLS protecting public.%', r.t;\n"
        "  END LOOP;\nEND\n$pgf$;";
    size_t n = strlen(PRE) + strlen(tenant_column) + strlen(SUF) + 1;
    char *sql = malloc(n);
    int rc = -1;
    if (sql) {
        snprintf(sql, n, "%s%s%s", PRE, tenant_column, SUF);
        rc = run_sql(c, sql);
        free(sql);
    }
    db_connection_release(c);
    if (rc == 0) LOG_INFO("tenancy-protect: RLS applied to all tables with column '%s'", tenant_column);
    return rc;
}

/* ---- tenant lifecycle (pooled mode) --------------------------------------- */

int pgf_tenant_create(const char *name, char *out_id, size_t out_sz) {
    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    const char *p[1] = { name };
    PGresult *r = PQexecParams(c, "INSERT INTO pgf_tenants(name) VALUES($1) RETURNING id::text",
                               1, NULL, p, NULL, NULL, 0);
    int rc = -1;
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) {
        snprintf(out_id, out_sz, "%s", PQgetvalue(r, 0, 0));
        rc = 0;
    } else {
        LOG_ERROR("create-tenant: %s", PQerrorMessage(c));
    }
    PQclear(r);
    db_connection_release(c);
    return rc;
}

int pgf_tenant_assign_user(const char *email, const char *tenant_id) {
    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    const char *p[2] = { tenant_id, email };
    PGresult *r = PQexecParams(c, "UPDATE pgf_users SET tenant_id=$1 WHERE email=$2",
                               2, NULL, p, NULL, NULL, 0);
    int rc = (PQresultStatus(r) == PGRES_COMMAND_OK && atoi(PQcmdTuples(r)) == 1) ? 0 : -1;
    if (rc != 0) LOG_ERROR("assign-user: no such user '%s' (or db error)", email);
    PQclear(r);
    db_connection_release(c);
    return rc;
}

int pgf_tenant_set_active(const char *name_or_id, int active, int *user_count) {
    if (user_count) *user_count = 0;
    PGconn *c = db_connection_acquire();
    if (!c) return -1;
    const char *act = active ? "true" : "false";
    int rc = -1;

    /* Flip the tenant flag (by name or id). RETURNING confirms it existed. */
    const char *p1[2] = { name_or_id, act };
    PGresult *r = PQexecParams(c,
        "UPDATE pgf_tenants SET is_active=$2 WHERE name=$1 OR id::text=$1 RETURNING id::text",
        2, NULL, p1, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        LOG_ERROR("tenant '%s' not found", name_or_id);
        PQclear(r); db_connection_release(c); return -1;
    }
    char tid[37]; snprintf(tid, sizeof tid, "%s", PQgetvalue(r, 0, 0));
    PQclear(r);

    /* Cascade to the tenant's users so the existing is_active auth check enforces
     * the suspension (a suspended tenant's users simply can't authenticate). */
    const char *p2[2] = { tid, act };
    r = PQexecParams(c, "UPDATE pgf_users SET is_active=$2 WHERE tenant_id=$1",
                     2, NULL, p2, NULL, NULL, 0);
    if (PQresultStatus(r) == PGRES_COMMAND_OK) {
        if (user_count) *user_count = atoi(PQcmdTuples(r));
        rc = 0;
    } else {
        LOG_ERROR("tenant set-active: %s", PQerrorMessage(c));
    }
    PQclear(r);
    db_connection_release(c);
    return rc;
}

/* Quoted, comma-separated column list for `table` excluding `skip`, in ordinal
 * order (e.g. `"id", "name"`). Caller frees; NULL on error/empty. */
static char *quoted_cols_except(PGconn *c, const char *table, const char *skip) {
    const char *p[2] = { table, skip };
    PGresult *r = PQexecParams(c,
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_schema='public' AND table_name=$1 AND column_name<>$2 "
        "ORDER BY ordinal_position", 2, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) { PQclear(r); return NULL; }
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (out) {
        out[0] = '\0';
        for (int i = 0; i < PQntuples(r); i++) {
            const char *col = PQgetvalue(r, i, 0);
            size_t need = len + strlen(col) + 8;
            if (need > cap) { cap = need * 2; char *nb = realloc(out, cap); if (!nb) { free(out); out = NULL; break; } out = nb; }
            len += (size_t)snprintf(out + len, cap - len, "%s\"%s\"", i ? ", " : "", col);
        }
    }
    PQclear(r);
    return out;
}

/* Emit `COPY <qtable> (<cols>) FROM stdin;` followed by the rows of
 * SELECT <cols> FROM <qtable> <where> in COPY text format, then `\.`. */
static int copy_out_table(PGconn *c, const char *qtable, const char *cols, const char *where) {
    /* L-8: size the statement dynamically — `cols` (the full quoted column list)
     * is unbounded, so a fixed 2 KB buffer silently truncated wide tables into an
     * invalid COPY (dropping the trailing ") TO STDOUT"). */
    if (!where) where = "";
    size_t need = strlen(cols) + strlen(qtable) + strlen(where) + 64;
    char *q = malloc(need);
    if (!q) { LOG_ERROR("export: out of memory"); return -1; }
    snprintf(q, need, "COPY (SELECT %s FROM %s %s) TO STDOUT", cols, qtable, where);
    PGresult *r = PQexec(c, q);
    free(q);
    if (PQresultStatus(r) != PGRES_COPY_OUT) {
        LOG_ERROR("export: %s", PQerrorMessage(c)); PQclear(r); return -1;
    }
    PQclear(r);
    printf("COPY %s (%s) FROM stdin;\n", qtable, cols);
    char *buf; int n;
    while ((n = PQgetCopyData(c, &buf, 0)) > 0) { fwrite(buf, 1, (size_t)n, stdout); PQfreemem(buf); }
    printf("\\.\n");
    r = PQgetResult(c);
    int ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
    PQclear(r);
    return ok ? 0 : -1;
}

int pgf_tenant_export(const char *name_or_id, const char *tenant_column) {
    if (!is_safe_ident(tenant_column)) return -1;
    PGconn *c = db_connection_acquire();
    if (!c) return -1;

    const char *p[1] = { name_or_id };
    PGresult *r = PQexecParams(c,
        "SELECT id::text, name FROM pgf_tenants WHERE name=$1 OR id::text=$1",
        1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        LOG_ERROR("export-tenant: tenant '%s' not found", name_or_id);
        PQclear(r); db_connection_release(c); return -1;
    }
    char tid[37];   snprintf(tid, sizeof tid, "%s", PQgetvalue(r, 0, 0));
    PQclear(r);
    char where[128];
    snprintf(where, sizeof where, "WHERE \"%s\"='%s'", tenant_column, tid);

    /* Header + a clean Model-A-friendly load (FK checks off, tenant column dropped). */
    printf("-- pgforge tenant export (graduate to a standalone Model A deployment).\n");
    printf("-- Load into a fresh DB after `pgforge migrate` + recreating the business\n");
    printf("-- tables WITHOUT the '%s' column:   psql -d <newdb> -f thisfile.sql\n", tenant_column);
    printf("BEGIN;\nSET session_replication_role = replica;\n\n");

    int rc = 0;
    /* the tenant's users first (without the tenant column) */
    char *ucols = quoted_cols_except(c, "pgf_users", tenant_column);
    if (ucols) { rc |= copy_out_table(c, "\"pgf_users\"", ucols, where); free(ucols); }

    /* their login credentials (pgf_identities has no tenant column — scope it via
     * user_id so graduated users can still authenticate). */
    char iwhere[160];
    snprintf(iwhere, sizeof iwhere,
             "WHERE user_id IN (SELECT id FROM \"pgf_users\" WHERE \"%s\"='%s')",
             tenant_column, tid);
    char *icols = quoted_cols_except(c, "pgf_identities", tenant_column);
    if (icols) { rc |= copy_out_table(c, "\"pgf_identities\"", icols, iwhere); free(icols); }

    /* then each tenant-scoped business table */
    const char *p2[1] = { tenant_column };
    r = PQexecParams(c,
        "SELECT c.table_name FROM information_schema.columns c "
        "JOIN information_schema.tables tb USING (table_schema, table_name) "
        "WHERE c.table_schema='public' AND c.column_name=$1 AND tb.table_type='BASE TABLE' "
        "AND left(c.table_name,4)<>'pgf_' ORDER BY c.table_name",
        1, NULL, p2, NULL, NULL, 0);
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            const char *tbl = PQgetvalue(r, i, 0);
            char *cols = quoted_cols_except(c, tbl, tenant_column);
            if (cols) {
                char qtbl[80]; snprintf(qtbl, sizeof qtbl, "\"%s\"", tbl);
                rc |= copy_out_table(c, qtbl, cols, where);
                free(cols);
            }
        }
    } else { rc = -1; }
    PQclear(r);

    printf("\nSET session_replication_role = origin;\nCOMMIT;\n");
    db_connection_release(c);
    return rc == 0 ? 0 : -1;
}

int pgf_migrate_status(int with_demo, int with_tenancy) {
    PGconn *c = db_connection_acquire();
    if (!c) { LOG_ERROR("migrate: no database connection"); return -1; }
    int rc = -1, done = 0, pending = 0;
    if (ensure_table(c) != 0) goto out;
    if (status_list(c, PGF_MIGRATIONS, PGF_MIGRATIONS_COUNT, "", &done, &pending) != 0) goto out;
    if (with_tenancy && status_list(c, PGF_TENANCY_MIGRATIONS, PGF_TENANCY_MIGRATIONS_COUNT, "tenancy:",
                                    &done, &pending) != 0) goto out;
    if (with_demo && status_list(c, PGF_DEMO_MIGRATIONS, PGF_DEMO_MIGRATIONS_COUNT, "demo:",
                                 &done, &pending) != 0) goto out;
    LOG_INFO("migrate: %d applied, %d pending", done, pending);
    rc = 0;
out:
    db_connection_release(c);
    return rc;
}
