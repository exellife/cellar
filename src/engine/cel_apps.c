#include "cel_apps.h"
#include "schema_catalog.h"
#include "cel_sync.h"
#include "core/app_db.h"
#include "core/auth_schema.h"
#include "cel_control.h"
#include "logger.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CEL_APPS_MAX 1024   /* distinct apps cached at once */

static struct {
    cel_app_t       apps[CEL_APPS_MAX];   /* fixed array → entry pointers are stable */
    int             count;
    char            apps_dir[1024];
    char            single_db[1024];
    bool            multi;
    bool            control;   /* CEL_CONTROL_DB configured → registry gates routing */
    pthread_mutex_t mtx;
} g = { .mtx = PTHREAD_MUTEX_INITIALIZER };

/* The host doubles as a directory name, so validate it tightly: lowercase, only
 * [a-z0-9.-], no leading dot/dash, no "..". Strips a trailing ":port". 0 on ok. */
int cel_apps_norm_host(const char *in, char *out, size_t n) {
    if (!in || !*in) return -1;
    size_t j = 0;
    for (const char *p = in; *p && *p != ':' && j < n - 1; p++) {
        char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-'))
            return -1;
        if (ch == '.' && p > in && p[-1] == '.') return -1;   /* no ".." */
        out[j++] = ch;
    }
    out[j] = '\0';
    if (j == 0 || out[0] == '.' || out[0] == '-') return -1;
    return 0;
}

void cel_apps_init(const char *apps_dir, const char *single_db) {
    pthread_mutex_lock(&g.mtx);
    if (apps_dir && *apps_dir) {
        snprintf(g.apps_dir, sizeof g.apps_dir, "%s", apps_dir);
        g.multi = true;
        LOG_INFO("apps: multi-app mode, bundles under %s", g.apps_dir);
        /* Optional control-plane registry: when CEL_CONTROL_DB is set, routing is
         * gated to registered + active hosts (an unregistered/suspended Host 404s). */
        const char *cdb = getenv("CEL_CONTROL_DB");
        if (cdb && *cdb && cel_control_open(cdb) == 0) {
            g.control = true;
            LOG_INFO("apps: control-plane registry at %s (routing gated)", cdb);
        }
    } else {
        snprintf(g.single_db, sizeof g.single_db, "%s", (single_db && *single_db) ? single_db : "cellar.db");
        g.multi = false;
        LOG_INFO("apps: single-app mode, database %s", g.single_db);
    }
    pthread_mutex_unlock(&g.mtx);
}

/* Caller holds g.mtx. */
static cel_app_t *find_cached(const char *host) {
    for (int i = 0; i < g.count; i++)
        if (!strcmp(g.apps[i].host, host)) return &g.apps[i];
    return NULL;
}

/* Open `db_path`, apply the identity schema, introspect the catalog. Caller holds
 * g.mtx. Returns a stable pointer into the cache, or NULL. */
/* Open the per-app background services (EventSink + JobQueue) on dedicated
 * connections to the app's data.db. Best-effort: on failure bg_ready stays 0 and
 * emit/enqueue become no-ops (telemetry/jobs degrade, the app still serves). */
static void open_bg_services(cel_app_t *slot, const char *db_path) {
    sqlite3 *ec = NULL, *jc = NULL;
    if (sqlite3_open(db_path, &ec) != SQLITE_OK) { sqlite3_close(ec); ec = NULL; }
    if (sqlite3_open(db_path, &jc) != SQLITE_OK) { sqlite3_close(jc); jc = NULL; }
    if (ec) { sqlite3_busy_timeout(ec, 5000); sqlite3_exec(ec, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;", NULL, NULL, NULL); }
    if (jc) { sqlite3_busy_timeout(jc, 5000); sqlite3_exec(jc, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;", NULL, NULL, NULL); }

    /* Open each adapter independently so a partial success can be torn down
     * cleanly. event_sink_sqlite_open caches a prepared INSERT on `ec`; if we
     * then bail without calling its destroy(), that statement is never finalized
     * and sqlite3_close(ec) returns BUSY → the ctx struct AND the connection both
     * leak (never freed; bg_ready stays 0 so shutdown skips them). Destroy any
     * adapter that opened before closing its connection. */
    int evt_ok = (ec && event_sink_sqlite_open(ec, &slot->events) == EVT_OK);
    int job_ok = (jc && job_queue_sqlite_open(jc, &slot->jobs) == JOBQ_OK);
    if (evt_ok && job_ok) {
        slot->evt_conn = ec; slot->jobq_conn = jc; slot->bg_ready = 1;
    } else {
        if (evt_ok) slot->events.destroy(slot->events.ctx);   /* finalizes c->ins */
        if (job_ok) slot->jobs.destroy(slot->jobs.ctx);
        slot->events = (event_sink_t){0};
        slot->jobs   = (job_queue_t){0};
        sqlite3_close(ec); sqlite3_close(jc);
        slot->bg_ready = 0;
        LOG_WARN("apps: background services (events/jobs) unavailable");
    }
}

static cel_app_t *open_into_cache(const char *host, const char *db_path) {
    if (g.count >= CEL_APPS_MAX) { LOG_WARN("apps: registry full (%d)", g.count); return NULL; }
    /* Pinned: this cache holds the handle for the process lifetime, so it must
     * never be LRU-evicted/freed underneath us (else a remote use-after-free
     * driveable by varying the Host header). */
    app_db_t *db = app_db_get_pinned(db_path);
    if (!db) return NULL;
    sqlite3 *c = app_db_conn_acquire(db);
    if (!c) return NULL;
    cel_auth_schema_apply(c);
    cel_catalog_t *cat = cel_catalog_build_sqlite(c);
    /* If any table opted into sync (carries rev + deleted), ensure this app's
     * monotonic rev source exists. _sync_seq is _%-prefixed → not in the catalog. */
    if (cat) {
        for (int i = 0; i < cat->ntables; i++)
            if (cat->tables[i].syncable) {
                if (cel_sync_ensure_seq(c) != 0)   /* late failure → first write 500s; log early */
                    LOG_WARN("apps: could not ensure _sync_seq for %s", host);
                break;
            }
    }
    app_db_conn_release(db, c);

    cel_app_t *slot = &g.apps[g.count++];
    snprintf(slot->host, sizeof slot->host, "%s", host);
    slot->db = db;
    slot->catalog = cat;

    /* the bundle dir is data.db's directory; hooks live in <dir>/hooks.lua and the
     * front-end in <dir>/public */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", db_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else snprintf(dir, sizeof dir, ".");
    snprintf(slot->bundle_dir, sizeof slot->bundle_dir, "%s", dir);
    slot->hooks = cel_hook_app_create(dir);

    /* per-app authorization policy from <dir>/policies.json (NULL → process default) */
    char pj[1100];
    snprintf(pj, sizeof pj, "%s/policies.json", dir);
    slot->policy = cel_policy_load(pj);

    open_bg_services(slot, db_path);

    return slot;
}

cel_app_t *cel_apps_resolve(const char *host) {
    /* single-app: one app serves every request, cached under a fixed key */
    if (!g.multi) {
        pthread_mutex_lock(&g.mtx);
        cel_app_t *app = find_cached("\x01" "default");
        if (!app) app = open_into_cache("\x01" "default", g.single_db);
        pthread_mutex_unlock(&g.mtx);
        return app;
    }

    char h[256];
    if (cel_apps_norm_host(host, h, sizeof h) != 0) return NULL;

    /* Control-plane gate (read per request so `cellar suspend` takes effect on a
     * running server): an unregistered or suspended host is never served. */
    if (g.control && !cel_control_is_active(h)) return NULL;

    pthread_mutex_lock(&g.mtx);
    cel_app_t *app = find_cached(h);
    if (!app) {
        char dir[1300];
        snprintf(dir, sizeof dir, "%s/%s", g.apps_dir, h);
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            char db[1400];
            snprintf(db, sizeof db, "%s/data.db", dir);
            app = open_into_cache(h, db);
        }
    }
    pthread_mutex_unlock(&g.mtx);
    return app;
}

cel_app_t *cel_apps_default(void) {
    return g.multi ? NULL : cel_apps_resolve(NULL);
}

/* This thread's current app + its hook metadata (mirrors the app_db / catalog
 * thread-local bindings cel_apps_enter installs). */
static __thread const cel_app_t *t_cur_app   = NULL;
static __thread cel_hook_app_t  *t_cur_hooks = NULL;

void cel_apps_enter(const cel_app_t *app) {
    app_db_set_current(app ? app->db : NULL);
    cel_catalog_set_active(app ? app->catalog : NULL);
    cel_policy_set_active(app ? app->policy : NULL);   /* NULL → process default */
    t_cur_app   = app;
    t_cur_hooks = app ? app->hooks : NULL;
}

void cel_apps_leave(void) {
    app_db_set_current(NULL);
    cel_catalog_set_active(NULL);
    cel_policy_clear_active();
    t_cur_app   = NULL;
    t_cur_hooks = NULL;
}

cel_hook_app_t  *cel_apps_current_hooks(void) { return t_cur_hooks; }
const cel_app_t *cel_apps_current(void)       { return t_cur_app; }

event_sink_t *cel_apps_current_events(void) {
    return (t_cur_app && t_cur_app->bg_ready) ? (event_sink_t *)&t_cur_app->events : NULL;
}
job_queue_t *cel_apps_current_jobs(void) {
    return (t_cur_app && t_cur_app->bg_ready) ? (job_queue_t *)&t_cur_app->jobs : NULL;
}

int cel_apps_snapshot(const cel_app_t **out, int max) {
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g.mtx);
    int n = g.count < max ? g.count : max;
    for (int i = 0; i < n; i++) out[i] = &g.apps[i];
    pthread_mutex_unlock(&g.mtx);
    return n;
}

void cel_apps_shutdown(void) {
    pthread_mutex_lock(&g.mtx);
    for (int i = 0; i < g.count; i++) {
        cel_catalog_free(g.apps[i].catalog);
        cel_hook_app_destroy(g.apps[i].hooks);
        cel_policy_free(g.apps[i].policy);
        if (g.apps[i].bg_ready) {
            g.apps[i].events.destroy(g.apps[i].events.ctx);
            g.apps[i].jobs.destroy(g.apps[i].jobs.ctx);
            sqlite3_close((sqlite3 *)g.apps[i].evt_conn);
            sqlite3_close((sqlite3 *)g.apps[i].jobq_conn);
            g.apps[i].bg_ready = 0;
        }
    }
    g.count = 0;
    pthread_mutex_unlock(&g.mtx);
    cel_control_close();
}
