/* ============================================================================
 * app_db_evict_test — regression for C-1/H-1 (cross-registry use-after-free).
 *
 * Compiled with -DCEL_APP_MAX_OPEN=4 so eviction triggers quickly. A *pinned*
 * handle (as the app registry holds it) must never be LRU-evicted/freed, even
 * though it is the least-recently-used: opening more apps than the cap must evict
 * an UNPINNED victim and leave the pinned handle valid and identical.
 * ============================================================================ */
#include "app_db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

int main(void) {
    char base[128]; snprintf(base, sizeof base, "/tmp/cellar_evict_%d", (int)getpid());
    char paths[8][160];
    for (int i = 0; i < 8; i++) snprintf(paths[i], sizeof paths[i], "%s_%d.db", base, i);
    for (int i = 0; i < 8; i++) { unlink(paths[i]); }

    app_db_global_init();

    /* A: opened first (oldest LRU) and PINNED, like the app registry caches it. */
    app_db_t *A = app_db_get_pinned(paths[0]);
    CHECK(A != NULL, "pinned app A opened");
    sqlite3 *c = app_db_conn_acquire(A);     /* materialize + return → checked_out back to 0 */
    CHECK(c != NULL, "A connection opens");
    app_db_conn_release(A, c);

    /* Fill the registry to the cap (4) with UNPINNED apps. A is now the LRU. */
    for (int i = 1; i < 4; i++) CHECK(app_db_get(paths[i]) != NULL, "fill registry (unpinned)");
    CHECK(app_db_open_count() == 4, "registry at cap (4)");

    /* Opening a 5th forces an eviction. Without the pin guard, A (the LRU) would
     * be the victim and get freed → cel_apps' cached pointer dangles (the bug).
     * With the fix an unpinned app is evicted and A survives. */
    app_db_t *E = app_db_get(paths[4]);
    CHECK(E != NULL, "5th app opened (an unpinned victim was evicted)");
    CHECK(app_db_open_count() == 4, "still 4 open (one evicted, not grown)");

    /* The pinned A must be the SAME live handle (not freed + reopened) and usable. */
    CHECK(app_db_get_pinned(paths[0]) == A, "pinned app A survived eviction (same pointer)");
    CHECK(strcmp(app_db_path(A), paths[0]) == 0, "A's path intact (not use-after-free)");
    sqlite3 *c2 = app_db_conn_acquire(A);
    CHECK(c2 != NULL, "pinned app A still usable after others evicted");
    app_db_conn_release(A, c2);

    /* Unref drops the pin; A then becomes evictable like any idle app. */
    app_db_unref(A); app_db_unref(A);   /* two pins taken (open + re-get) */
    app_db_t *F = app_db_get(paths[5]);
    CHECK(F != NULL, "open another after unpinning A");

    app_db_global_shutdown();
    for (int i = 0; i < 8; i++) {
        char x[200]; unlink(paths[i]);
        snprintf(x, sizeof x, "%s-wal", paths[i]); unlink(x);
        snprintf(x, sizeof x, "%s-shm", paths[i]); unlink(x);
    }
    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall app_db_evict checks passed\n");
    return 0;
}
