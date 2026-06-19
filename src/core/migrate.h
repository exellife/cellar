/* ============================================================================
 * pgforge — database migration runner.
 *
 * Migrations are embedded into the binary and applied in filename order, each in
 * its own transaction, recorded in pgf_migrations (with a checksum) so the
 * process is idempotent and edited-after-apply migrations are refused. The whole
 * run is serialized by a Postgres advisory lock so concurrent migrators are safe.
 *
 * Sets: core (sql/migrations/, always), demo (sql/demo/, opt-in sample data,
 * recorded under "demo:"), and tenancy (sql/tenancy/, opt-in pooled multi-tenant
 * schema, recorded under "tenancy:").
 * ============================================================================ */
#ifndef PGF_MIGRATE_H
#define PGF_MIGRATE_H

#include <stddef.h>   /* size_t */

typedef struct {
    const char *name;   /* migration filename, e.g. "001_users_sessions.sql" */
    const char *sql;    /* its full SQL text */
} pgf_migration_t;

extern const pgf_migration_t PGF_MIGRATIONS[];              /* core */
extern const int             PGF_MIGRATIONS_COUNT;
extern const pgf_migration_t PGF_DEMO_MIGRATIONS[];         /* demo sample data */
extern const int             PGF_DEMO_MIGRATIONS_COUNT;
extern const pgf_migration_t PGF_TENANCY_MIGRATIONS[];      /* pooled tenancy schema */
extern const int             PGF_TENANCY_MIGRATIONS_COUNT;

/* Apply pending core migrations (plus tenancy if with_tenancy, plus demo if
 * with_demo). Advisory-locked, transactional, idempotent, checksum-verified.
 * *applied_out (may be NULL) gets the count applied. Returns 0 / -1. */
int pgf_migrate_run(int with_demo, int with_tenancy, int *applied_out);

/* Print applied vs pending (core, plus tenancy/demo if requested). Returns 0/-1. */
int pgf_migrate_status(int with_demo, int with_tenancy);

/* Enable Postgres RLS + the tenant-isolation policy on every public table that
 * carries `tenant_column` (pooled-mode defense-in-depth). Idempotent; run it
 * again after adding new tenant-scoped tables. Returns 0/-1. */
int pgf_tenancy_protect(const char *tenant_column);

/* ---- tenant lifecycle (pooled mode) --------------------------------------- */

/* Insert a tenant; writes its UUID (text) to out_id. Returns 0/-1. */
int pgf_tenant_create(const char *name, char *out_id, size_t out_sz);
/* Set an existing user's tenant_id. Returns 0/-1 (-1 if no such user). */
int pgf_tenant_assign_user(const char *email, const char *tenant_id);
/* Activate/suspend a tenant (by name or id) and cascade is_active to its users,
 * so a suspended tenant's users can no longer authenticate. *user_count (may be
 * NULL) gets the number of users affected. Returns 0/-1 (-1 if tenant absent). */
int pgf_tenant_set_active(const char *name_or_id, int active, int *user_count);

/* Write a loadable SQL script to stdout containing this tenant's users and
 * tenant-scoped business rows (the tenant column dropped) — the "graduate to a
 * standalone Model A deployment" escape hatch. Returns 0/-1. Logs go to stderr;
 * keep stdout for the script. */
int pgf_tenant_export(const char *name_or_id, const char *tenant_column);

#endif /* PGF_MIGRATE_H */
