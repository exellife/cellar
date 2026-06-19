/* ============================================================================
 * cellar — per-app SQLite handle layer
 *
 * The foundation of the SQLite engine (Phase 1). Where pgforge held a single
 * pool of Postgres connections to one shared database, cellar holds *one SQLite
 * file per app* and reaches each app only through its own handles — the OS file
 * boundary is the isolation (no tenant_id, no RLS).
 *
 * Concurrency contract (design §6): SQLite is synchronous and single-writer per
 * file; portico is a non-blocking event loop. Therefore every call here is meant
 * to run on a DB worker thread, never the event loop. This module provides:
 *
 *   - open-by-file, lazily, with WAL mode (many readers + one writer);
 *   - a small per-app pool of `sqlite3*` handles → concurrent readers;
 *   - per-app write serialization → one write-hot app waits on *its own* file
 *     lock without starving others, and writers never collide into SQLITE_BUSY;
 *   - a bounded LRU of open apps → thousands of bundles can't exhaust FDs.
 *
 * Handles are opened SQLITE_OPEN_NOMUTEX: each is used by exactly one thread at
 * a time, guaranteed by the acquire/release pool — so SQLite does no internal
 * locking on the hot path.
 * ============================================================================ */
#ifndef CEL_APP_DB_H
#define CEL_APP_DB_H

#include <sqlite3.h>
#include <stdbool.h>

/* Tunables. CONNS_PER_APP caps concurrent handles (hence readers) for one app;
 * MAX_OPEN_APPS bounds how many app files are held open at once (FD ceiling). */
#ifndef CEL_APP_CONNS_PER_APP
#define CEL_APP_CONNS_PER_APP 4
#endif
#ifndef CEL_APP_MAX_OPEN
#define CEL_APP_MAX_OPEN 64
#endif

/* Opaque per-app handle (its file path, connection pool, and write lock). */
typedef struct app_db app_db_t;

/* Initialize the global app registry. Idempotent. Returns 0 on success. */
int  app_db_global_init(void);

/* Close every open app and free the registry. Safe to call once at shutdown. */
void app_db_global_shutdown(void);

/* Get (or lazily create) the app whose database file is `db_path`. The file is
 * not opened until the first connection is acquired. The returned pointer is
 * owned by the registry and stays valid until shutdown or LRU eviction (an app
 * with no checked-out connections may be evicted to honor CEL_APP_MAX_OPEN).
 * Returns NULL only if the registry is full of in-use apps. */
app_db_t *app_db_get(const char *db_path);

/* Borrow a WAL-mode `sqlite3*` for this app, opening one on demand (up to
 * CEL_APP_CONNS_PER_APP). Blocks while the pool is exhausted. Returns NULL if
 * the file cannot be opened/configured. Pair every success with a release. */
sqlite3 *app_db_conn_acquire(app_db_t *db);
void     app_db_conn_release(app_db_t *db, sqlite3 *conn);

/* Per-app writer serialization. Hold this across a write/DDL/transaction so a
 * single app's writers run one at a time (SQLite is single-writer per file);
 * different apps never block each other. Readers do NOT take this lock. */
void app_db_write_lock(app_db_t *db);
void app_db_write_unlock(app_db_t *db);

/* Convenience for a write/DDL statement (or several, ';'-separated): takes the
 * write lock, borrows a connection, runs sqlite3_exec, then releases both.
 * Returns an SQLite result code; on error and if `errmsg` is non-NULL, *errmsg
 * receives a malloc'd message the caller must sqlite3_free(). */
int app_db_exec(app_db_t *db, const char *sql, char **errmsg);

/* The app's database file path (for logging / diagnostics). */
const char *app_db_path(const app_db_t *db);

/* Number of apps currently held open (for metrics / tests). */
int app_db_open_count(void);

#endif /* CEL_APP_DB_H */
