#include "cel_sync.h"
#include "logger.h"

#include <sqlite3.h>

int cel_sync_ensure_seq(sqlite3 *c) {
    /* CHECK(id=1) pins it to a single row; INSERT OR IGNORE seeds seq=0 the first
     * time and is a no-op on every subsequent open. */
    char *err = NULL;
    int rc = sqlite3_exec(c,
        "CREATE TABLE IF NOT EXISTS _sync_seq("
        "  id  INTEGER PRIMARY KEY CHECK(id = 1),"
        "  seq INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO _sync_seq(id, seq) VALUES (1, 0);",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sync: ensure _sync_seq failed: %s", err ? err : sqlite3_errmsg(c));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

long long cel_sync_next_rev(sqlite3 *c) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c,
            "UPDATE _sync_seq SET seq = seq + 1 WHERE id = 1 RETURNING seq",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_ERROR("sync: next_rev prepare failed: %s", sqlite3_errmsg(c));
        return -1;
    }
    long long rev = -1;
    if (sqlite3_step(st) == SQLITE_ROW) rev = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (rev < 0) LOG_ERROR("sync: next_rev produced no row (is _sync_seq seeded?)");
    return rev;
}
