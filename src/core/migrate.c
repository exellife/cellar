#include "migrate.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

/* 64-bit FNV-1a over the file bytes → 16 hex chars. Enough to notice an
 * accidental edit of an already-applied migration (drift); not a crypto guard. */
static void fnv1a_hex(const unsigned char *p, size_t n, char out[17]) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    snprintf(out, 17, "%016llx", h);
}

static int ensure_table(sqlite3 *db, char *err, size_t errlen) {
    char *e = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS _schema_migrations("
        "  id         TEXT PRIMARY KEY,"
        "  checksum   TEXT NOT NULL,"
        "  applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ','now')))",
        NULL, NULL, &e);
    if (rc != SQLITE_OK) { snprintf(err, errlen, "tracker table: %s", e ? e : "?"); sqlite3_free(e); return -1; }
    return 0;
}

/* Read a whole file into a malloc'd, NUL-terminated buffer; *len = byte length. */
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

static int has_sql_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".sql") == 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Look up an already-applied migration. Returns 0 if found (sets *match to
 * whether the recorded checksum equals `checksum`), 1 if not found, -1 on error. */
static int lookup(sqlite3 *db, const char *id, const char *checksum, int *match) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT checksum FROM _schema_migrations WHERE id=?", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st), ret;
    if (rc == SQLITE_ROW) {
        const char *got = (const char *)sqlite3_column_text(st, 0);
        *match = (got && strcmp(got, checksum) == 0);
        ret = 0;
    } else {
        ret = (rc == SQLITE_DONE) ? 1 : -1;
    }
    sqlite3_finalize(st);
    return ret;
}

/* Apply one migration + record it, atomically (own transaction). 0 on success. */
static int apply_one(sqlite3 *db, const char *id, const char *sql, const char *checksum,
                     char *err, size_t errlen) {
    char *e = NULL;
    if (sqlite3_exec(db, "BEGIN", NULL, NULL, &e) != SQLITE_OK) {
        snprintf(err, errlen, "%s: BEGIN: %s", id, e ? e : "?"); sqlite3_free(e); return -1;
    }
    if (sqlite3_exec(db, sql, NULL, NULL, &e) != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", id, e ? e : "?"); sqlite3_free(e);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "INSERT INTO _schema_migrations(id, checksum) VALUES(?,?)", -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "%s: record: %s", id, sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, checksum, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        snprintf(err, errlen, "%s: record: %s", id, sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &e) != SQLITE_OK) {
        snprintf(err, errlen, "%s: COMMIT: %s", id, e ? e : "?"); sqlite3_free(e);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

int cel_migrate_run(sqlite3 *db, const char *dir, cel_migrate_result_t *out) {
    cel_migrate_result_t r;
    memset(&r, 0, sizeof r);
    if (!db || !dir) { snprintf(r.err, sizeof r.err, "null argument"); if (out) *out = r; return -1; }

    if (ensure_table(db, r.err, sizeof r.err) != 0) { if (out) *out = r; return -1; }

    DIR *d = opendir(dir);
    if (!d) { if (out) *out = r; return 0; }   /* no migrations dir → nothing to do */

    /* collect *.sql names, sort lexically */
    char **names = NULL; int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || !has_sql_suffix(de->d_name)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, (size_t)cap * sizeof *names); }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    r.total = n;

    int rc = 0;
    for (int i = 0; i < n; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        size_t len = 0;
        char *sql = read_file(path, &len);
        if (!sql) { snprintf(r.err, sizeof r.err, "%s: cannot read", names[i]); rc = -1; break; }
        char checksum[17];
        fnv1a_hex((const unsigned char *)sql, len, checksum);

        int match = 0;
        int found = lookup(db, names[i], checksum, &match);
        if (found < 0) { snprintf(r.err, sizeof r.err, "%s: lookup failed", names[i]); free(sql); rc = -1; break; }
        if (found == 0) {                       /* already applied */
            if (!match) {
                snprintf(r.err, sizeof r.err, "%s: checksum mismatch (edited after apply)", names[i]);
                free(sql); rc = -1; break;
            }
            r.skipped++; free(sql); continue;
        }
        if (apply_one(db, names[i], sql, checksum, r.err, sizeof r.err) != 0) { free(sql); rc = -1; break; }
        r.applied++; free(sql);
    }

    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    if (out) *out = r;
    return rc;
}
