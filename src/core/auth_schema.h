/* ============================================================================
 * cellar — the auth/identity schema for an app's SQLite database.
 *
 * Design §11: per-app users live in each app's data.db; the auth tables ship to
 * every app as a "base migration". This is the SQLite-native, consolidated form
 * of the old Postgres migrations (final state, not replayed history): ids
 * are TEXT uuids (minted in C — SQLite has no gen_random_uuid()), timestamps are
 * INTEGER unix-epoch seconds, booleans are INTEGER 0/1.
 * ============================================================================ */
#ifndef CEL_AUTH_SCHEMA_H
#define CEL_AUTH_SCHEMA_H

struct sqlite3;

/* Create the cel_* auth/identity tables in `db` if absent (idempotent —
 * CREATE TABLE IF NOT EXISTS). Returns 0 on success, -1 on error. */
int cel_auth_schema_apply(struct sqlite3 *db);

#endif /* CEL_AUTH_SCHEMA_H */
