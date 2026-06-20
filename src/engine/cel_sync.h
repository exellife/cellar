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

#include <stddef.h>   /* size_t */

struct sqlite3;   /* forward-declared so includers needn't pull in <sqlite3.h> */

/* Ensure the per-app rev source `_sync_seq` exists and is seeded to 0. Idempotent;
 * call once per app that has any syncable table (at open). Returns 0 on success. */
int cel_sync_ensure_seq(struct sqlite3 *c);

/* Allocate this app's next revision: `UPDATE _sync_seq SET seq=seq+1 RETURNING seq`.
 * Runs on the given connection, so when called inside the request transaction the
 * bump commits — or rolls back — atomically with the write it stamps (a failed or
 * denied write consumes no rev). Returns the new rev (>= 1), or -1 on error. */
long long cel_sync_next_rev(struct sqlite3 *c);

/* The app's current high-water rev (the last allocated value), or 0 if `_sync_seq`
 * is absent/empty. Used as the cursor a client should advance to after a push. */
long long cel_sync_current_seq(struct sqlite3 *c);

/* Idempotent-retry dedup. cel_sync_applied_get: 1 (+ fills status_out/rev_out) if a
 * mutation_id was already applied, 0 if not, -1 on error. cel_sync_applied_put:
 * record a mutation's result (idempotency key) in the current txn; 0 on success. */
int cel_sync_applied_get(struct sqlite3 *c, const char *mutation_id,
                         char *status_out, size_t status_len, long long *rev_out);
int cel_sync_applied_put(struct sqlite3 *c, const char *mutation_id,
                         const char *status, long long rev);

#endif /* CEL_SYNC_H */
