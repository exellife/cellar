/* ============================================================================
 * cel_sync_test — the per-app rev source (sync Slice 0, T2).
 *
 * Checks _sync_seq seeding, monotonic rev allocation, idempotent ensure, and
 * that a rolled-back transaction consumes no rev (the bump rides the surrounding
 * txn). In-memory SQLite, libpq-free.
 * ============================================================================ */
#include "cel_sync.h"

#include <sqlite3.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

static long long seq_of(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    long long v = -1;
    if (sqlite3_prepare_v2(db, "SELECT seq FROM _sync_seq WHERE id=1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

int main(void) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open in-memory db");

    /* ensure seeds a single row at 0 */
    CHECK(cel_sync_ensure_seq(db) == 0, "ensure _sync_seq");
    CHECK(seq_of(db) == 0, "seeded seq == 0");

    /* monotonic allocation */
    CHECK(cel_sync_next_rev(db) == 1, "next_rev -> 1");
    CHECK(cel_sync_next_rev(db) == 2, "next_rev -> 2");
    CHECK(cel_sync_next_rev(db) == 3, "next_rev -> 3");
    CHECK(seq_of(db) == 3, "seq persisted at 3");

    /* ensure is idempotent and does NOT reset the counter */
    CHECK(cel_sync_ensure_seq(db) == 0, "ensure again (idempotent)");
    CHECK(seq_of(db) == 3, "counter survives re-ensure");

    /* a rolled-back txn consumes no rev (no gaps from failed writes) */
    CHECK(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK, "begin txn");
    CHECK(cel_sync_next_rev(db) == 4, "next_rev -> 4 inside txn");
    CHECK(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK, "rollback txn");
    CHECK(seq_of(db) == 3, "rolled-back bump did not stick (still 3)");
    CHECK(cel_sync_next_rev(db) == 4, "next_rev -> 4 again (no gap)");

    sqlite3_close(db);
    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall cel_sync checks passed\n");
    return 0;
}
