/* cellar — background job worker. See cel_worker.h. */
#include "cel_worker.h"
#include "cel_apps.h"
#include "cel_hooks.h"
#include "cel_hook_state.h"
#include "core/app_db.h"
#include "logger.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>

#define WORKER_MAX_APPS   512
#define WORKER_BUDGET     200    /* jobs per app per tick */
#define WORKER_VISIBILITY 60     /* secs a claim is held before reclaim */

static pthread_t   g_thread;
static _Atomic int g_run = 0;
static int         g_interval = 0;

/* Run one app's due jobs — mirrors the POST /jobs/run handler: bind the app + a
 * connection + the write lock so handlers serialize with request writes. The
 * worker thread gets its own per-(thread,app) Lua state via cel_hook_app_state.
 * Concurrency with request threads is safe: the EventSink/JobQueue module
 * mutexes serialize access to their dedicated connections. */
static void run_app_jobs(const cel_app_t *app) {
    cel_apps_enter(app);
    job_queue_t *q = cel_apps_current_jobs();
    cel_lua_t   *L = q ? cel_hook_app_state(cel_apps_current_hooks()) : NULL;
    if (q && L) {
        app_db_t *adb = app_db_current();
        sqlite3  *c   = adb ? app_db_conn_acquire(adb) : NULL;
        if (adb) app_db_write_lock(adb);
        cel_hooks_set_db(c);
        int n = cel_hooks_run_jobs(L, q, (long long)time(NULL), WORKER_VISIBILITY, WORKER_BUDGET);
        cel_hooks_set_db(NULL);
        if (adb) { app_db_write_unlock(adb); app_db_conn_release(adb, c); }
        if (n > 0) LOG_DEBUG("job worker: ran %d job(s) for %s", n, app->host);
    }
    cel_apps_leave();
}

static void *worker_loop(void *arg) {
    (void)arg;
    LOG_INFO("job worker: running every %ds", g_interval);
    while (atomic_load(&g_run)) {
        const cel_app_t *apps[WORKER_MAX_APPS];
        int n = cel_apps_snapshot(apps, WORKER_MAX_APPS);
        for (int i = 0; i < n && atomic_load(&g_run); i++) run_app_jobs(apps[i]);
        /* sleep in 1s steps so stop is responsive */
        for (int s = 0; s < g_interval && atomic_load(&g_run); s++) sleep(1);
    }
    cel_hook_state_thread_cleanup();   /* free this thread's lazily-compiled hook states */
    return NULL;
}

void cel_worker_start(int interval_secs) {
    if (interval_secs <= 0) { LOG_INFO("job worker: disabled (CEL_JOBS_INTERVAL=0)"); return; }
    g_interval = interval_secs;
    atomic_store(&g_run, 1);
    if (pthread_create(&g_thread, NULL, worker_loop, NULL) != 0) {
        atomic_store(&g_run, 0);
        LOG_ERROR("job worker: failed to start");
    }
}

void cel_worker_stop(void) {
    if (!atomic_exchange(&g_run, 0)) return;   /* not running */
    pthread_join(g_thread, NULL);
}
