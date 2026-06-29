/* Unit test for cel_migrate_run — the bundle app-schema migration runner.
 * DB-free of cellar internals: drives the runner against an in-memory SQLite DB
 * and throwaway migration dirs under /tmp. */
#include "migrate.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void chk(int cond, const char *label) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

static void write_file(const char *dir, const char *name, const char *content) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(content, f); fclose(f); }
}
static void rm_file(const char *dir, const char *name) {
    char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, name); unlink(p);
}

static int table_exists(sqlite3 *db, const char *t) {
    sqlite3_stmt *st; int e = 0;
    sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, t, -1, SQLITE_TRANSIENT);
    e = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return e;
}
static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st; int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

int main(void) {
    cel_migrate_result_t r;
    int rc;

    /* ---- A) happy path: apply, idempotent re-run, incremental add ---------- */
    char dir[] = "/tmp/cel-migrate-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    sqlite3 *db; sqlite3_open(":memory:", &db);

    write_file(dir, "0001_init.sql", "CREATE TABLE t1(id INTEGER PRIMARY KEY);");
    write_file(dir, "0002_add.sql",  "CREATE TABLE t2(id INTEGER);");

    rc = cel_migrate_run(db, dir, &r);
    chk(rc == 0, "apply: rc == 0");
    chk(r.applied == 2 && r.skipped == 0 && r.total == 2, "apply: 2 applied / 0 skipped / 2 total");
    chk(table_exists(db, "t1") && table_exists(db, "t2"), "apply: t1 + t2 created");
    chk(scalar(db, "SELECT count(*) FROM _schema_migrations") == 2, "apply: 2 rows recorded");

    rc = cel_migrate_run(db, dir, &r);
    chk(rc == 0 && r.applied == 0 && r.skipped == 2, "idempotent: 0 applied / 2 skipped");

    write_file(dir, "0003_more.sql", "CREATE TABLE t3(id INTEGER);");
    rc = cel_migrate_run(db, dir, &r);
    chk(rc == 0 && r.applied == 1 && r.skipped == 2 && r.total == 3, "incremental: only the new one applies");
    chk(table_exists(db, "t3"), "incremental: t3 created");

    /* ---- B) drift: an already-applied migration edited after the fact ------- */
    write_file(dir, "0001_init.sql", "CREATE TABLE t1(id INTEGER PRIMARY KEY, extra TEXT);");
    rc = cel_migrate_run(db, dir, &r);
    chk(rc != 0, "drift: rc != 0");
    chk(strstr(r.err, "checksum") != NULL, "drift: checksum-mismatch error");
    sqlite3_close(db);

    /* ---- C) a failing migration rolls back AND is not recorded ------------- */
    char dir2[] = "/tmp/cel-migrate2-XXXXXX";
    if (!mkdtemp(dir2)) { perror("mkdtemp"); return 2; }
    sqlite3 *db2; sqlite3_open(":memory:", &db2);
    write_file(dir2, "0001_ok.sql",  "CREATE TABLE a(id INTEGER);");
    /* table b is created, then a bad statement fails → the whole migration rolls back */
    write_file(dir2, "0002_bad.sql", "CREATE TABLE b(id INTEGER); INSERT INTO nope VALUES(1);");
    rc = cel_migrate_run(db2, dir2, &r);
    chk(rc != 0, "failure: rc != 0");
    chk(r.applied == 1, "failure: 0001 applied before the bad one");
    chk(table_exists(db2, "a"), "failure: a (0001) present");
    chk(!table_exists(db2, "b"), "failure: b rolled back (0002 txn atomic)");
    chk(scalar(db2, "SELECT count(*) FROM _schema_migrations WHERE id='0002_bad.sql'") == 0,
        "failure: 0002 not recorded");
    sqlite3_close(db2);

    /* ---- D) edge cases: missing dir is a no-op; null args error ------------ */
    sqlite3 *db3; sqlite3_open(":memory:", &db3);
    rc = cel_migrate_run(db3, "/tmp/cel-migrate-nope-zzzz", &r);
    chk(rc == 0 && r.total == 0, "missing dir: no-op (rc 0, total 0)");
    chk(cel_migrate_run(NULL, dir, &r) != 0, "null db: error");
    sqlite3_close(db3);

    /* cleanup temp files/dirs */
    rm_file(dir, "0001_init.sql"); rm_file(dir, "0002_add.sql"); rm_file(dir, "0003_more.sql"); rmdir(dir);
    rm_file(dir2, "0001_ok.sql"); rm_file(dir2, "0002_bad.sql"); rmdir(dir2);

    printf("\n%s (%d checks failed)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
