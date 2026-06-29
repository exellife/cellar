/* ============================================================================
 * cel_migrate — app-schema migration runner (bundle `migrations/` → data.db).
 *
 * The engine evolves its OWN identity tables via PRAGMA user_version
 * (auth_schema.c). A bundle's app schema can't reuse that (user_version is
 * engine-owned), so it carries ordered, forward-only `*.sql` files and this
 * runner applies the ones a given data.db hasn't seen yet — tracked in a
 * `_schema_migrations` table. See docs/app-bundle.md §4.
 * ============================================================================ */
#ifndef CEL_MIGRATE_H
#define CEL_MIGRATE_H

#include <stddef.h>

struct sqlite3;

/* Outcome of a migration run (counts + an error message on failure). */
typedef struct {
    int  total;     /* migration files discovered in the dir */
    int  applied;   /* newly applied this run */
    int  skipped;   /* already applied (checksum matched) */
    char err[256];  /* human-readable message on failure; empty on success */
} cel_migrate_result_t;

/* Apply pending schema migrations from `dir` to the open database `db`.
 *
 * Migration files are the `*.sql` entries in `dir`, applied in ascending
 * filename order (use a zero-padded numeric prefix, e.g. `0001_init.sql`).
 * Each not-yet-applied file is run in its OWN transaction together with the
 * bookkeeping insert, so a migration is applied if and only if it is recorded
 * (atomic). An already-applied file is verified against its recorded checksum:
 * a mismatch (the file was edited after it was applied) is a hard error.
 *
 * Tracking lives in `_schema_migrations(id, checksum, applied_at)` (created on
 * first run). A missing `dir` is not an error (nothing to do).
 *
 * Returns 0 if all pending migrations applied cleanly (or there were none);
 * non-zero on the first failure — that migration is rolled back, but migrations
 * applied earlier in the same run stay committed. `*out` (optional) receives the
 * counts and, on failure, the message. Migration files must NOT contain their
 * own BEGIN/COMMIT. */
int cel_migrate_run(struct sqlite3 *db, const char *dir, cel_migrate_result_t *out);

#endif /* CEL_MIGRATE_H */
