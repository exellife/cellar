/* cellar — the control-plane registry (design §11). See cel_control.h. */
#include "cel_control.h"
#include "logger.h"

#include <sqlite3.h>
#include <pthread.h>
#include <string.h>

/* One process-wide connection to the (single, shared) control DB. The server
 * holds it open and reads status per request; CLIs open it briefly. Guarded by a
 * mutex — routing calls this concurrently from worker threads. */
static sqlite3         *g_db  = NULL;
static pthread_mutex_t  g_mtx = PTHREAD_MUTEX_INITIALIZER;

int cel_control_open(const char *path) {
    pthread_mutex_lock(&g_mtx);
    if (g_db) { pthread_mutex_unlock(&g_mtx); return 0; }
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        LOG_ERROR("control: open '%s' failed: %s", path, g_db ? sqlite3_errmsg(g_db) : "?");
        if (g_db) { sqlite3_close(g_db); g_db = NULL; }
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    sqlite3_busy_timeout(g_db, 5000);
    char *err = NULL;
    sqlite3_exec(g_db,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS cel_registry("
        "  host       TEXT PRIMARY KEY,"
        "  status     TEXT NOT NULL DEFAULT 'active',"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
        ");", NULL, NULL, &err);
    if (err) { LOG_ERROR("control: schema: %s", err); sqlite3_free(err); }
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void cel_control_close(void) {
    pthread_mutex_lock(&g_mtx);
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
    pthread_mutex_unlock(&g_mtx);
}

/* Prepare `sql`, bind host as ?1 (+ optional ?2 text), step to DONE; returns
 * sqlite3_changes() or -1. Caller holds g_mtx. */
static int exec_host(const char *sql, const char *host, const char *p2) {
    if (!g_db) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, host, -1, SQLITE_TRANSIENT);
    if (p2) sqlite3_bind_text(st, 2, p2, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(g_db);
}

int cel_control_register(const char *host) {
    pthread_mutex_lock(&g_mtx);
    int rc = exec_host("INSERT OR IGNORE INTO cel_registry(host) VALUES(?1)", host, NULL);
    pthread_mutex_unlock(&g_mtx);
    return rc < 0 ? -1 : 0;
}

int cel_control_set_status(const char *host, const char *status) {
    pthread_mutex_lock(&g_mtx);
    int rc = exec_host("UPDATE cel_registry SET status=?2 WHERE host=?1", host, status);
    pthread_mutex_unlock(&g_mtx);
    return rc;   /* rows changed (0 = no such host), or -1 */
}

int cel_control_unregister(const char *host) {
    pthread_mutex_lock(&g_mtx);
    int rc = exec_host("DELETE FROM cel_registry WHERE host=?1", host, NULL);
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

bool cel_control_is_active(const char *host) {
    pthread_mutex_lock(&g_mtx);
    bool active = false;
    sqlite3_stmt *st = NULL;
    if (g_db && sqlite3_prepare_v2(g_db, "SELECT status FROM cel_registry WHERE host=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, host, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *s = (const char *)sqlite3_column_text(st, 0);
            active = s && strcmp(s, "active") == 0;
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mtx);
    return active;
}

void cel_control_list(void (*cb)(const char *, const char *, long long, void *), void *ud) {
    pthread_mutex_lock(&g_mtx);
    sqlite3_stmt *st = NULL;
    if (g_db && sqlite3_prepare_v2(g_db,
            "SELECT host, status, created_at FROM cel_registry ORDER BY host", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            cb((const char *)sqlite3_column_text(st, 0),
               (const char *)sqlite3_column_text(st, 1),
               (long long)sqlite3_column_int64(st, 2), ud);
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_mtx);
}
