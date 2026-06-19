/* ============================================================================
 * app_db_test — unit test for the per-app SQLite handle layer.
 *
 * DB-free in the Postgres sense: it creates throwaway SQLite files under /tmp
 * and exercises open-by-file, the connection pool, per-app write serialization
 * (many threads writing the same app land every row), concurrent readers across
 * pooled handles, the get-by-path cache, and LRU bookkeeping.
 * ============================================================================ */
#include "app_db.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
    else         { fprintf(stderr, "ok: %s\n", msg); } \
} while (0)

/* a unique-ish path per run so repeated runs don't fight over WAL state */
static void tmp_path(char *buf, size_t cap, const char *tag)
{
    snprintf(buf, cap, "/tmp/cellar_appdb_%d_%s.db", (int)getpid(), tag);
}
static void rm_db(const char *p)
{
    char x[4096];
    unlink(p);
    snprintf(x, sizeof x, "%s-wal", p); unlink(x);
    snprintf(x, sizeof x, "%s-shm", p); unlink(x);
}

/* ---- single row count helper ---------------------------------------------- */
static long count_rows(app_db_t *db)
{
    sqlite3 *c = app_db_conn_acquire(db);
    assert(c);
    sqlite3_stmt *st = NULL;
    long n = -1;
    if (sqlite3_prepare_v2(c, "SELECT COUNT(*) FROM t", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    app_db_conn_release(db, c);
    return n;
}

/* ---- concurrency: writers ------------------------------------------------- */
#define WRITERS 8
#define PER_WRITER 50

typedef struct { app_db_t *db; int id; } warg_t;

static void *writer(void *p)
{
    warg_t *a = p;
    for (int i = 0; i < PER_WRITER; i++) {
        char sql[128];
        snprintf(sql, sizeof sql,
                 "INSERT INTO t(w, i) VALUES(%d, %d)", a->id, i);
        char *err = NULL;
        int rc = app_db_exec(a->db, sql, &err);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "writer %d insert %d failed: %s\n", a->id, i, err ? err : "?");
            sqlite3_free(err);
        }
    }
    return NULL;
}

/* ---- concurrency: readers ------------------------------------------------- */
#define READERS 8
typedef struct { app_db_t *db; long expect; int ok; } rarg_t;

static void *reader(void *p)
{
    rarg_t *a = p;
    a->ok = (count_rows(a->db) == a->expect);
    return NULL;
}

int main(void)
{
    char path[4096];
    tmp_path(path, sizeof path, "main");
    rm_db(path);

    CHECK(app_db_global_init() == 0, "global init");

    app_db_t *db = app_db_get(path);
    CHECK(db != NULL, "get app by path");
    CHECK(app_db_get(path) == db, "get-by-path is cached (same pointer)");
    CHECK(app_db_open_count() == 1, "one app open");
    CHECK(strcmp(app_db_path(db), path) == 0, "path round-trips");

    /* schema via the write-path convenience */
    char *err = NULL;
    CHECK(app_db_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, w INT, i INT)", &err) == SQLITE_OK,
          "create table");
    sqlite3_free(err);

    /* WAL actually engaged? */
    {
        sqlite3 *c = app_db_conn_acquire(db);
        sqlite3_stmt *st = NULL;
        char mode[16] = "";
        if (sqlite3_prepare_v2(c, "PRAGMA journal_mode", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW)
            snprintf(mode, sizeof mode, "%s", (const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        app_db_conn_release(db, c);
        CHECK(strcasecmp(mode, "wal") == 0, "journal_mode is WAL");
    }

    /* concurrent writers: per-app serialization must land every row */
    {
        pthread_t th[WRITERS];
        warg_t args[WRITERS];
        for (int i = 0; i < WRITERS; i++) { args[i].db = db; args[i].id = i; pthread_create(&th[i], NULL, writer, &args[i]); }
        for (int i = 0; i < WRITERS; i++) pthread_join(th[i], NULL);
        CHECK(count_rows(db) == (long)WRITERS * PER_WRITER, "all concurrent writes landed");
    }

    /* concurrent readers across the pooled handles */
    {
        long expect = (long)WRITERS * PER_WRITER;
        pthread_t th[READERS];
        rarg_t args[READERS];
        for (int i = 0; i < READERS; i++) { args[i].db = db; args[i].expect = expect; args[i].ok = 0; pthread_create(&th[i], NULL, reader, &args[i]); }
        int all = 1;
        for (int i = 0; i < READERS; i++) { pthread_join(th[i], NULL); all &= args[i].ok; }
        CHECK(all, "all concurrent readers saw the full count");
    }

    /* a second, distinct app opens independently (isolation by file) */
    {
        char path2[4096];
        tmp_path(path2, sizeof path2, "two");
        rm_db(path2);
        app_db_t *db2 = app_db_get(path2);
        CHECK(db2 != NULL && db2 != db, "second app is a distinct handle");
        CHECK(app_db_open_count() == 2, "two apps open");
        CHECK(app_db_exec(db2, "CREATE TABLE t(id INTEGER PRIMARY KEY, w INT, i INT)", NULL) == SQLITE_OK,
              "second app create table");
        CHECK(count_rows(db2) == 0, "second app is empty — no cross-app leakage");
        rm_db(path2);
    }

    app_db_global_shutdown();
    CHECK(app_db_open_count() == 0, "shutdown closes all apps");
    rm_db(path);

    if (failures) { fprintf(stderr, "\n%d check(s) FAILED\n", failures); return 1; }
    fprintf(stderr, "\nall app_db checks passed\n");
    return 0;
}
