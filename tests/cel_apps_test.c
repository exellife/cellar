/* ============================================================================
 * cel_apps_test — the app registry + Host routing.
 *
 * single-app mode (any Host → the one app) and multi-app mode (Host → bundle dir),
 * plus :port stripping, unknown/invalid-host rejection, per-app catalogs, and the
 * thread-local enter/leave binding. Creates throwaway bundle dirs under /tmp; no
 * network, no Postgres.
 * ============================================================================ */
#include "cel_apps.h"
#include "schema_catalog.h"
#include "core/app_db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

/* create <dir>/data.db with one table, so the catalog has something distinctive */
static void make_app(const char *dir, const char *table) {
    mkdir(dir, 0755);
    char path[512]; snprintf(path, sizeof path, "%s/data.db", dir);
    sqlite3 *db = NULL;
    sqlite3_open(path, &db);
    char sql[256]; snprintf(sql, sizeof sql, "CREATE TABLE %s(id INTEGER PRIMARY KEY, x TEXT)", table);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
}

int main(void) {
    char base[256]; snprintf(base, sizeof base, "/tmp/cellar_apps_%d", (int)getpid());
    char single[300]; snprintf(single, sizeof single, "%s_single.db", base);
    unlink(single);
    mkdir(base, 0755);
    char a[320], b[320];
    snprintf(a, sizeof a, "%s/a.com", base);
    snprintf(b, sizeof b, "%s/b.com", base);
    make_app(a, "alpha");
    make_app(b, "beta");
    { sqlite3 *db; sqlite3_open(single, &db); sqlite3_exec(db, "CREATE TABLE solo(id INTEGER PRIMARY KEY)", 0,0,0); sqlite3_close(db); }

    app_db_global_init();

    /* ---- single-app mode: any Host resolves to the one app ---- */
    cel_apps_init(NULL, single);
    cel_app_t *d = cel_apps_default();
    CHECK(d != NULL, "single-app: default app exists");
    CHECK(d && cel_catalog_find(d->catalog, "solo"), "single-app: catalog is the single db");
    CHECK(cel_apps_resolve("anything.com") == d, "single-app: any Host -> the one app");
    CHECK(cel_apps_resolve("other.org:9000") == d, "single-app: Host ignored, same app");

    /* ---- multi-app mode: Host -> bundle dir ---- */
    cel_apps_init(base, NULL);
    CHECK(cel_apps_default() == NULL, "multi-app: no default app");

    cel_app_t *app_a = cel_apps_resolve("a.com");
    CHECK(app_a != NULL, "resolve a.com");
    CHECK(app_a && cel_catalog_find(app_a->catalog, "alpha"), "a.com catalog has 'alpha'");
    CHECK(app_a && !cel_catalog_find(app_a->catalog, "beta"), "a.com catalog has no 'beta'");

    cel_app_t *app_b = cel_apps_resolve("b.com");
    CHECK(app_b != NULL && app_b != app_a, "resolve b.com (distinct app)");
    CHECK(app_b && cel_catalog_find(app_b->catalog, "beta"), "b.com catalog has 'beta'");

    CHECK(cel_apps_resolve("A.com") == app_a, "Host is case-insensitive");
    CHECK(cel_apps_resolve("a.com:8080") == app_a, "trailing :port stripped, cache hit");
    CHECK(cel_apps_resolve("nope.com") == NULL, "unknown Host -> NULL (no bundle dir)");
    CHECK(cel_apps_resolve("../etc") == NULL, "path-traversal Host rejected");
    CHECK(cel_apps_resolve("a/b") == NULL, "Host with slash rejected");
    CHECK(cel_apps_resolve("") == NULL, "empty Host rejected");

    /* ---- enter/leave binds the calling thread's current app + catalog ---- */
    cel_apps_enter(app_a);
    CHECK(app_db_current() == app_a->db, "enter binds app_db_current()");
    CHECK(cel_catalog_active() == app_a->catalog, "enter binds cel_catalog_active()");
    cel_apps_enter(app_b);
    CHECK(cel_catalog_active() == app_b->catalog, "re-enter rebinds to b");
    cel_apps_leave();
    CHECK(app_db_current() == NULL, "leave clears the binding (no default in multi-app)");

    cel_apps_shutdown();
    app_db_global_shutdown();

    /* cleanup */
    char p[400];
    snprintf(p, sizeof p, "%s/data.db", a); unlink(p);
    snprintf(p, sizeof p, "%s/data.db", b); unlink(p);
    rmdir(a); rmdir(b); unlink(single); rmdir(base);

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall cel_apps checks passed\n");
    return 0;
}
