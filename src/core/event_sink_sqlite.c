/* ============================================================================
 * cellar — EventSink SQLite adapter (default). See event_sink.h.
 *
 * Appends each event to an `event` table (AUTOINCREMENT id = a gap-free-enough
 * monotonic cursor even across deletes). A cached INSERT statement keeps emit on
 * the hot path; an internal mutex makes emit/read safe on a single NOMUTEX
 * connection (cellar opens its app connections NOMUTEX).
 * ============================================================================ */
#include "event_sink.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sqlite3        *db;          /* caller-owned */
    pthread_mutex_t mu;
    sqlite3_stmt   *ins;         /* cached INSERT */
} evt_ctx;

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS event ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts         INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  type       TEXT NOT NULL,"
    "  actor_id   TEXT,"
    "  subject_id TEXT,"
    "  props      TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_event_type_ts ON event(type, ts);"
    "CREATE INDEX IF NOT EXISTS idx_event_subject ON event(subject_id, ts);";

/* bind a nullable text param (NULL -> SQL NULL). */
static int bind_opt(sqlite3_stmt *s, int i, const char *v) {
    return v ? sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT)
             : sqlite3_bind_null(s, i);
}

static int evt_emit(void *vctx, const event_t *ev) {
    evt_ctx *c = vctx;
    if (!ev || !ev->type || !ev->type[0]) return EVT_EINVAL;

    pthread_mutex_lock(&c->mu);
    sqlite3_reset(c->ins);
    sqlite3_clear_bindings(c->ins);
    int rc = EVT_OK;
    if (sqlite3_bind_text(c->ins, 1, ev->type, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        bind_opt(c->ins, 2, ev->actor_id)   != SQLITE_OK ||
        bind_opt(c->ins, 3, ev->subject_id) != SQLITE_OK ||
        bind_opt(c->ins, 4, ev->props)      != SQLITE_OK) {
        rc = EVT_EIO;
    } else if (sqlite3_step(c->ins) != SQLITE_DONE) {
        rc = EVT_EIO;
    }
    sqlite3_reset(c->ins);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static long long evt_read(void *vctx, long long after_id, int limit,
                          event_visit_fn visit, void *user) {
    evt_ctx *c = vctx;
    if (!visit || limit <= 0) return EVT_EINVAL;

    pthread_mutex_lock(&c->mu);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
            "SELECT id, ts, type, actor_id, subject_id, props "
            "FROM event WHERE id > ? ORDER BY id LIMIT ?", -1, &st, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&c->mu);
        return EVT_EIO;
    }
    sqlite3_bind_int64(st, 1, after_id);
    sqlite3_bind_int(st, 2, limit);

    long long n = 0;
    int step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        event_t ev = {
            .type       = (const char *)sqlite3_column_text(st, 2),
            .actor_id   = (const char *)sqlite3_column_text(st, 3),
            .subject_id = (const char *)sqlite3_column_text(st, 4),
            .props      = (const char *)sqlite3_column_text(st, 5),
        };
        long long id = sqlite3_column_int64(st, 0);
        long long ts = sqlite3_column_int64(st, 1);
        n++;
        if (visit(user, id, ts, &ev) != 0) break;   /* caller asked to stop */
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&c->mu);
    if (step != SQLITE_ROW && step != SQLITE_DONE) return EVT_EIO;
    return n;
}

static void evt_destroy(void *vctx) {
    evt_ctx *c = vctx;
    if (!c) return;
    if (c->ins) sqlite3_finalize(c->ins);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

int event_sink_sqlite_open(void *db, event_sink_t *out) {
    if (!db || !out) return EVT_EINVAL;
    sqlite3 *sdb = (sqlite3 *)db;

    if (sqlite3_exec(sdb, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) return EVT_EIO;

    evt_ctx *c = calloc(1, sizeof *c);
    if (!c) return EVT_ENOMEM;
    c->db = sdb;
    if (pthread_mutex_init(&c->mu, NULL) != 0) { free(c); return EVT_EIO; }

    if (sqlite3_prepare_v2(sdb,
            "INSERT INTO event(type, actor_id, subject_id, props) VALUES(?,?,?,?)",
            -1, &c->ins, NULL) != SQLITE_OK) {
        pthread_mutex_destroy(&c->mu); free(c);
        return EVT_EIO;
    }

    out->ctx     = c;
    out->emit    = evt_emit;
    out->read    = evt_read;
    out->destroy = evt_destroy;
    return EVT_OK;
}
