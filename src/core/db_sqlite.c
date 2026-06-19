#include "db_sqlite.h"

#include <sqlite3.h>
#include <stdio.h>
#include <time.h>

int cel_db_prep(sqlite3 *c, const char *sql, const char *const *p, int n, sqlite3_stmt **out) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(c, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { *out = NULL; return rc; }
    for (int i = 0; i < n; i++) {
        if (p[i]) sqlite3_bind_text(st, i + 1, p[i], -1, SQLITE_TRANSIENT);
        else      sqlite3_bind_null(st, i + 1);
    }
    *out = st;
    return SQLITE_OK;
}

bool cel_db_exec(sqlite3 *c, const char *sql, const char *const *p, int n) {
    sqlite3_stmt *st;
    if (cel_db_prep(c, sql, p, n, &st) != SQLITE_OK) return false;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

int cel_db_one_text(sqlite3 *c, const char *sql, const char *const *p, int n,
                    char *out, size_t out_size) {
    sqlite3_stmt *st;
    if (cel_db_prep(c, sql, p, n, &st) != SQLITE_OK) return -1;
    int rc = sqlite3_step(st);
    int ret;
    if (rc == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        snprintf(out, out_size, "%s", v ? (const char *)v : "");
        ret = 1;
    } else if (rc == SQLITE_DONE) {
        ret = 0;
    } else {
        ret = -1;
    }
    sqlite3_finalize(st);
    return ret;
}

long cel_now_epoch(void) { return (long)time(NULL); }
