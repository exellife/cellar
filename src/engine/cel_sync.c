#include "cel_sync.h"
#include "logger.h"

#include <sqlite3.h>

int cel_sync_ensure_seq(sqlite3 *c) {
    /* The engine-internal sync bookkeeping (all _%-prefixed → never in the catalog):
     *   _sync_seq     — the per-app monotonic rev counter (one pinned row).
     *   _sync_applied — idempotency keys: a mutation_id maps to its result so a
     *                   retried push is a no-op (no re-apply, no rev bump).
     *   _sync_devices — each device's high-water cursor (for tombstone GC). */
    char *err = NULL;
    int rc = sqlite3_exec(c,
        "CREATE TABLE IF NOT EXISTS _sync_seq("
        "  id  INTEGER PRIMARY KEY CHECK(id = 1),"
        "  seq INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO _sync_seq(id, seq) VALUES (1, 0);"
        "CREATE TABLE IF NOT EXISTS _sync_applied("
        "  mutation_id TEXT PRIMARY KEY,"
        "  tbl         TEXT    NOT NULL,"   /* key also bound to (table,id) so a reused */
        "  row_id      TEXT    NOT NULL,"   /* mutation_id for a different row isn't deduped */
        "  status      TEXT    NOT NULL,"
        "  rev         INTEGER NOT NULL,"
        "  at          INTEGER NOT NULL);"
        /* user-scoped: a device belongs to its owner, so one user can never touch (and
         * force-advance the cursor of) another user's device row. */
        "CREATE TABLE IF NOT EXISTS _sync_devices("
        "  user_id   TEXT    NOT NULL,"
        "  device_id TEXT    NOT NULL,"
        "  cursor    INTEGER NOT NULL,"
        "  seen_at   INTEGER NOT NULL,"
        "  PRIMARY KEY (user_id, device_id));",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sync: ensure _sync_* failed: %s", err ? err : sqlite3_errmsg(c));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int cel_sync_applied_get(sqlite3 *c, const char *mutation_id, const char *tbl, const char *row_id,
                         char *status_out, size_t status_len, long long *rev_out) {
    sqlite3_stmt *st = NULL;
    /* Bound to (mutation_id, table, id): a mutation_id reused for a DIFFERENT row does
     * not match here, so it applies normally instead of being silently dropped. */
    if (sqlite3_prepare_v2(c,
            "SELECT status, rev FROM _sync_applied "
            "WHERE mutation_id = ?1 AND tbl = ?2 AND row_id = ?3", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, mutation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, tbl, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, row_id, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = 1;
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (status_out && status_len) snprintf(status_out, status_len, "%s", s ? s : "applied");
        if (rev_out) *rev_out = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return found;
}

int cel_sync_device_seen(sqlite3 *c, const char *device_id, const char *user_id, long long cursor) {
    sqlite3_stmt *st = NULL;
    /* Upsert; cursor only ever advances (MAX) so a stale/parallel pull can't rewind it. */
    if (sqlite3_prepare_v2(c,
            "INSERT INTO _sync_devices(user_id, device_id, cursor, seen_at) "
            "VALUES (?1, ?2, ?3, unixepoch()) "
            "ON CONFLICT(user_id, device_id) DO UPDATE SET "
            "  cursor = MAX(cursor, excluded.cursor), seen_at = excluded.seen_at", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, cursor);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int cel_sync_applied_put(sqlite3 *c, const char *mutation_id, const char *tbl, const char *row_id,
                         const char *status, long long rev) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c,
            "INSERT OR IGNORE INTO _sync_applied(mutation_id, tbl, row_id, status, rev, at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, unixepoch())", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, mutation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, tbl, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, row_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, rev);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
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

long long cel_sync_min_device_cursor(sqlite3 *c, long long active_since) {
    sqlite3_stmt *st = NULL;
    /* Only ACTIVE devices (seen_at >= active_since) gate GC. A device abandoned past
     * the staleness horizon stops pinning the min — it'll full-resync when it returns,
     * so its tombstones needn't be kept forever (and a fake/idle device can't block GC
     * fleet-wide indefinitely). */
    if (sqlite3_prepare_v2(c, "SELECT count(*), MIN(cursor) FROM _sync_devices WHERE seen_at >= ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, active_since);
    long long v = -1;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0)
        v = sqlite3_column_int64(st, 1);   /* -1 (no active devices) → caller purges nothing */
    sqlite3_finalize(st);
    return v;
}

/* A bare SQLite identifier ([A-Za-z_][A-Za-z0-9_]*) — gc_table interpolates the table
 * name, so refuse anything that isn't one (defense-in-depth; catalog names are trusted). */
static int sync_safe_ident(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    return 1;
}

int cel_sync_gc_table(sqlite3 *c, const char *table, long long min_cursor) {
    if (min_cursor < 0) return 0;   /* no active devices → purge nothing (conservative) */
    if (!sync_safe_ident(table)) return -1;
    char sql[256];
    snprintf(sql, sizeof sql, "DELETE FROM \"%s\" WHERE deleted = 1 AND rev <= ?1", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, min_cursor);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? sqlite3_changes(c) : -1;
}

int cel_sync_prune_applied(sqlite3 *c, long long before_epoch) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c, "DELETE FROM _sync_applied WHERE at < ?1", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, before_epoch);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? sqlite3_changes(c) : -1;
}

long long cel_sync_current_seq(sqlite3 *c) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c, "SELECT seq FROM _sync_seq WHERE id = 1", -1, &st, NULL) != SQLITE_OK)
        return 0;
    long long seq = 0;
    if (sqlite3_step(st) == SQLITE_ROW) seq = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return seq;
}
