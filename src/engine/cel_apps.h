/* ============================================================================
 * cellar — the app registry + request routing (design §4-5).
 *
 * One cellar process hosts many apps. Each request is routed to an app by its
 * Host header (the routing key is the full hostname — a.com, b.com, shop.x.com),
 * which maps to a bundle directory: <apps_dir>/<host>/data.db. Provisioning is
 * "drop a directory"; the bundle's data.db is opened lazily on first request,
 * its identity schema applied, and its catalog introspected and cached.
 *
 * A single-app mode (no apps_dir, just one data.db) is the simple deployment and
 * the fallback the WebSocket path uses until the handshake Host is plumbed
 * through the transport (the opcode layer carries no Host today).
 * ============================================================================ */
#ifndef CEL_APPS_H
#define CEL_APPS_H

#include "schema_catalog.h"
#include "core/app_db.h"
#include "core/event_sink.h"
#include "core/job_queue.h"
#include "cel_hook_state.h"
#include "policy.h"

/* A resolved app: its SQLite handle, its (cached) introspected catalog, its hook
 * metadata (the bundle's hooks.lua, compiled lazily per worker thread), and its
 * authorization policy (the bundle's policies.json; NULL = process default). */
typedef struct {
    char            host[256];
    char            bundle_dir[1024];   /* the bundle directory (data.db's dir); <dir>/public is the front-end */
    app_db_t       *db;
    cel_catalog_t  *catalog;
    cel_hook_app_t *hooks;
    cel_policy_t   *policy;
    /* Background services (EventSink + JobQueue), each on its own dedicated
     * connection to this app's data.db, opened eagerly at app-open. NULL conns
     * mean the services failed to open (emit/enqueue then no-op). */
    void           *evt_conn;           /* sqlite3* */
    void           *jobq_conn;          /* sqlite3* */
    event_sink_t    events;
    job_queue_t     jobs;
    int             bg_ready;
} cel_app_t;

/* Configure routing. If `apps_dir` is non-empty → multi-app (<apps_dir>/<host>/
 * data.db per Host). Otherwise → single-app: one `single_db` for every request
 * (defaults to "cellar.db" if single_db is NULL/empty). */
void cel_apps_init(const char *apps_dir, const char *single_db);

/* Validate + normalize a Host into a safe bundle directory name (lowercase,
 * [a-z0-9.-], no leading dot/dash, no ".."; a trailing ":port" is stripped).
 * Returns 0 and writes `out` on success, non-zero if the host is unusable. Used
 * by routing and by the provisioning CLI. */
int cel_apps_norm_host(const char *in, char *out, size_t n);

/* Resolve a Host header value (":port" tolerated; case-insensitive) to its app,
 * opening + caching it on first use. Returns NULL if the host is invalid or no
 * such bundle exists (→ the caller should 404). */
cel_app_t *cel_apps_resolve(const char *host);

/* The default app for transports without a Host (the WS path): the single app in
 * single-app mode, or NULL in multi-app mode. */
cel_app_t *cel_apps_default(void);

/* Bind `app` (or NULL) as the calling thread's current app + active catalog for
 * the duration of a request; cel_apps_leave() clears the binding (falls back to
 * the process default). */
void cel_apps_enter(const cel_app_t *app);
void cel_apps_leave(void);

/* The hook metadata of this thread's current app (set by cel_apps_enter), or NULL
 * outside a bound request / when the app has none. */
cel_hook_app_t *cel_apps_current_hooks(void);

/* This thread's current app (set by cel_apps_enter), or NULL outside a bound
 * request. Used by the router to find the app's public/ front-end directory. */
const cel_app_t *cel_apps_current(void);

/* This thread's current app's background services (each on a dedicated
 * connection), or NULL if unavailable. Used by the hook FFI (cellar.emit /
 * cellar.enqueue_job / cellar.run_jobs). */
event_sink_t *cel_apps_current_events(void);
job_queue_t  *cel_apps_current_jobs(void);

/* Snapshot up to `max` currently-open app slot pointers into `out` (the slots are
 * stable for the process lifetime). Returns the count. Used by the background job
 * worker to iterate apps without holding the registry lock during job runs. */
int cel_apps_snapshot(const cel_app_t **out, int max);

/* Free the cached catalogs (handles are freed by app_db_global_shutdown). */
void cel_apps_shutdown(void);

#endif /* CEL_APPS_H */
