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
 * MAX_OPEN_APPS bounds how many app files are held open at once. It must be >= the
 * app registry's cap (cel_apps CEL_APPS_MAX) because every routed app is *pinned*
 * (referenced) here and so is never LRU-evicted — the LRU only reclaims unpinned,
 * idle handles (e.g. transient direct app_db_get callers / tests). */
/* MUST exceed the POOL_DB worker-thread count (see main.c dispatcher config).
 * INVARIANT — a request handler can hold TWO pool connections at once: an /rpc
 * request binds one conn across the whole hook (cel_api_rpc, for cellar.query/exec)
 * while a nested auth primitive it calls (cellar.create_user / set_password /
 * set_user_active / delete_user) acquires a SECOND. If the pool == worker count,
 * N threads each holding their rpc conn and each blocking on a nested acquire would
 * deadlock (acquire waits on a release that can't happen until the hook returns).
 * Sizing the pool at 2x the worker count lets every worker hold both conns without
 * contention, so nested acquires never block, let alone deadlock. Bump this if the
 * POOL_DB thread_count grows. */
#ifndef CEL_APP_CONNS_PER_APP
#define CEL_APP_CONNS_PER_APP 8
#endif
#ifndef CEL_APP_MAX_OPEN
#define CEL_APP_MAX_OPEN 1024
#endif

/* Opaque per-app handle (its file path, connection pool, and write lock). */
typedef struct app_db app_db_t;

/* Initialize the global app registry. Idempotent. Returns 0 on success. */
int  app_db_global_init(void);

/* Close every open app and free the registry. Safe to call once at shutdown. */
void app_db_global_shutdown(void);

/* The app a query runs against. app_db_current() returns this thread's binding if
 * set (app_db_set_current, by per-request routing), else the process-wide default
 * (app_db_set_default, set once at boot for the single-app deployment), else NULL.
 * The binding is thread-local; clear it (set NULL) at the end of a request to fall
 * back to the default. */
void      app_db_set_current(app_db_t *db);   /* per-thread request binding */
app_db_t *app_db_current(void);
void      app_db_set_default(app_db_t *db);   /* process-wide fallback */

/* Get (or lazily create) the app whose database file is `db_path`. The file is
 * not opened until the first connection is acquired. The returned pointer stays
 * valid until shutdown or LRU eviction (an UNPINNED app with no checked-out
 * connections may be evicted to honor CEL_APP_MAX_OPEN). Returns NULL only if the
 * registry is full of pinned/in-use apps. */
app_db_t *app_db_get(const char *db_path);

/* Like app_db_get but takes a long-lived REFERENCE: the handle will never be
 * LRU-evicted/freed while any reference is outstanding. A holder that caches the
 * pointer across requests (the app registry) MUST use this — otherwise eviction
 * can free a handle still referenced or in-flight on another thread (use-after-
 * free). Balance each call with app_db_unref (or process shutdown). */
app_db_t *app_db_get_pinned(const char *db_path);
void      app_db_unref(app_db_t *db);

/* Borrow a WAL-mode `sqlite3*` for this app, opening one on demand (up to
 * CEL_APP_CONNS_PER_APP). Blocks while the pool is exhausted. Returns NULL if
 * the file cannot be opened/configured. Pair every success with a release. */
sqlite3 *app_db_conn_acquire(app_db_t *db);
void     app_db_conn_release(app_db_t *db, sqlite3 *conn);

/* Return a prepared statement for `sql` on the borrowed connection `conn`, from a
 * PER-CONNECTION cache keyed by the SQL text (parse + plan happen once per distinct
 * statement, not once per request — the major per-request cost on the CRUD path).
 * The returned statement is reset with its bindings cleared, ready to bind+step;
 * the caller must NOT finalize it (the cache owns it and finalizes at connection
 * close) — just sqlite3_reset it when done. NULL on a compile error. `conn` must be
 * one currently checked out by this thread. */
sqlite3_stmt *app_db_stmt_cached(app_db_t *db, sqlite3 *conn, const char *sql);

/* Distinct prepared statements cached per connection (FIFO-evicted past this). */
#ifndef CEL_STMT_CACHE_MAX
#define CEL_STMT_CACHE_MAX 64
#endif

/* Per-app writer serialization. Hold this across a write/DDL/transaction so a
 * single app's writers run one at a time (SQLite is single-writer per file);
 * different apps never block each other. Readers do NOT take this lock. */
void app_db_write_lock(app_db_t *db);
void app_db_write_unlock(app_db_t *db);
/* True if the calling thread currently holds an app write lock (CRUD before/after/
 * resolve hooks + job hooks run under it). Lets a re-entrant write path detect +
 * refuse re-acquiring the non-recursive mutex — e.g. cellar.create_user from a
 * write hook (only the rpc path, which holds no write lock, is safe). */
int  app_db_in_write_lock(void);

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
