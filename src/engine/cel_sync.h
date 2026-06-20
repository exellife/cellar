/* ============================================================================
 * cellar — offline-first sync substrate (design: cellar-sync-design.md).
 *
 * Slice 0: the per-app monotonic revision source. A *syncable* table (one that
 * declares both a `rev` and a `deleted` column — see schema_catalog) gets every
 * write stamped with the next `rev` from a single per-app counter, persisted in
 * the engine-internal `_sync_seq` table. That total order is the cursor the
 * pull/push protocol (later slices) builds on.
 * ============================================================================ */
#ifndef CEL_SYNC_H
#define CEL_SYNC_H

struct sqlite3;   /* forward-declared so includers needn't pull in <sqlite3.h> */

/* Ensure the per-app rev source `_sync_seq` exists and is seeded to 0. Idempotent;
 * call once per app that has any syncable table (at open). Returns 0 on success. */
int cel_sync_ensure_seq(struct sqlite3 *c);

/* Allocate this app's next revision: `UPDATE _sync_seq SET seq=seq+1 RETURNING seq`.
 * Runs on the given connection, so when called inside the request transaction the
 * bump commits — or rolls back — atomically with the write it stamps (a failed or
 * denied write consumes no rev). Returns the new rev (>= 1), or -1 on error. */
long long cel_sync_next_rev(struct sqlite3 *c);

#endif /* CEL_SYNC_H */
