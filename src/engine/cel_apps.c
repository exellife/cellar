#include "cel_apps.h"
#include "schema_catalog.h"
#include "core/app_db.h"
#include "core/auth_schema.h"
#include "logger.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CEL_APPS_MAX 1024   /* distinct apps cached at once */

static struct {
    cel_app_t       apps[CEL_APPS_MAX];   /* fixed array → entry pointers are stable */
    int             count;
    char            apps_dir[1024];
    char            single_db[1024];
    bool            multi;
    pthread_mutex_t mtx;
} g = { .mtx = PTHREAD_MUTEX_INITIALIZER };

/* The host doubles as a directory name, so validate it tightly: lowercase, only
 * [a-z0-9.-], no leading dot/dash, no "..". Strips a trailing ":port". 0 on ok. */
static int norm_host(const char *in, char *out, size_t n) {
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
static cel_app_t *open_into_cache(const char *host, const char *db_path) {
    if (g.count >= CEL_APPS_MAX) { LOG_WARN("apps: registry full (%d)", g.count); return NULL; }
    app_db_t *db = app_db_get(db_path);
    if (!db) return NULL;
    sqlite3 *c = app_db_conn_acquire(db);
    if (!c) return NULL;
    cel_auth_schema_apply(c);
    cel_catalog_t *cat = cel_catalog_build_sqlite(c);
    app_db_conn_release(db, c);

    cel_app_t *slot = &g.apps[g.count++];
    snprintf(slot->host, sizeof slot->host, "%s", host);
    slot->db = db;
    slot->catalog = cat;
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
    if (norm_host(host, h, sizeof h) != 0) return NULL;

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

void cel_apps_enter(const cel_app_t *app) {
    app_db_set_current(app ? app->db : NULL);
    cel_catalog_set_active(app ? app->catalog : NULL);
}

void cel_apps_leave(void) {
    app_db_set_current(NULL);
    cel_catalog_set_active(NULL);
}

void cel_apps_shutdown(void) {
    pthread_mutex_lock(&g.mtx);
    for (int i = 0; i < g.count; i++) cel_catalog_free(g.apps[i].catalog);
    g.count = 0;
    pthread_mutex_unlock(&g.mtx);
}
