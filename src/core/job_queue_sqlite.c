/* ============================================================================
 * cellar — JobQueue SQLite adapter (default). See job_queue.h.
 *
 * The claim is a single UPDATE…RETURNING against a subquery, so it is atomic
 * under concurrency (SQLite serializes writers): two workers can never claim the
 * same job. A claim sets state='claimed' + claimed_at; a claim older than the
 * visibility window is reclaimable (crash-safety). complete deletes a one-shot
 * or re-schedules a recurring job; fail retries until max_attempts then
 * dead-letters.
 * ============================================================================ */
#include "job_queue.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_ATTEMPTS 5

typedef struct {
    sqlite3        *db;
    pthread_mutex_t mu;
} jq_ctx;

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS job ("
    "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  type         TEXT NOT NULL,"
    "  payload      TEXT,"
    "  state        TEXT NOT NULL DEFAULT 'pending',"   /* pending|claimed|dead */
    "  run_at       INTEGER NOT NULL DEFAULT 0,"
    "  attempt      INTEGER NOT NULL DEFAULT 0,"
    "  max_attempts INTEGER NOT NULL DEFAULT 5,"
    "  repeat_every INTEGER NOT NULL DEFAULT 0,"
    "  claimed_at   INTEGER,"
    "  last_error   TEXT,"
    "  created_at   INTEGER NOT NULL DEFAULT (unixepoch()));"
    "CREATE INDEX IF NOT EXISTS idx_job_due ON job(state, run_at);";

static char *dupz(const char *s) { return s ? strdup(s) : NULL; }

static int jq_enqueue(void *vctx, const char *type, const char *payload,
                      long long run_at, int max_attempts, long long repeat_every,
                      long long *out_id) {
    jq_ctx *c = vctx;
    if (!type || !type[0]) return JOBQ_EINVAL;
    if (max_attempts <= 0) max_attempts = DEFAULT_MAX_ATTEMPTS;

    pthread_mutex_lock(&c->mu);
    sqlite3_stmt *st = NULL;
    int rc = JOBQ_OK;
    if (sqlite3_prepare_v2(c->db,
            "INSERT INTO job(type, payload, run_at, max_attempts, repeat_every) "
            "VALUES(?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK) { rc = JOBQ_EIO; goto out; }
    sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
    payload ? sqlite3_bind_text(st, 2, payload, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 2);
    sqlite3_bind_int64(st, 3, run_at);
    sqlite3_bind_int(st, 4, max_attempts);
    sqlite3_bind_int64(st, 5, repeat_every);
    if (sqlite3_step(st) != SQLITE_DONE) rc = JOBQ_EIO;
    else if (out_id) *out_id = sqlite3_last_insert_rowid(c->db);
out:
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static int jq_claim(void *vctx, long long now, int visibility, job_t *job) {
    jq_ctx *c = vctx;
    if (!job) return JOBQ_EINVAL;
    memset(job, 0, sizeof *job);

    pthread_mutex_lock(&c->mu);
    sqlite3_stmt *st = NULL;
    int rc = JOBQ_NONE;
    /* Atomic claim: pick the oldest due pending job OR a stuck claim past its
     * visibility window; flip to claimed, bump attempt, RETURNING the row. */
    if (sqlite3_prepare_v2(c->db,
            "UPDATE job SET state='claimed', claimed_at=?1, attempt=attempt+1 "
            "WHERE id = (SELECT id FROM job WHERE "
            "   (state='pending' AND run_at <= ?1) OR "
            "   (state='claimed' AND claimed_at <= ?1 - ?2) "
            "   ORDER BY run_at LIMIT 1) "
            "RETURNING id, type, payload, attempt, max_attempts", -1, &st, NULL) != SQLITE_OK) {
        rc = JOBQ_EIO; goto out;
    }
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int(st, 2, visibility);
    if (sqlite3_step(st) == SQLITE_ROW) {
        job->id           = sqlite3_column_int64(st, 0);
        job->type         = dupz((const char *)sqlite3_column_text(st, 1));
        job->payload      = dupz((const char *)sqlite3_column_text(st, 2));
        job->attempt      = sqlite3_column_int(st, 3);
        job->max_attempts = sqlite3_column_int(st, 4);
        rc = JOBQ_OK;
    }
out:
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static int jq_complete(void *vctx, long long id, long long now) {
    jq_ctx *c = vctx;
    pthread_mutex_lock(&c->mu);
    int rc = JOBQ_OK;
    sqlite3_stmt *st = NULL;
    /* recurring: re-schedule at now + repeat_every; one-shot: delete. */
    if (sqlite3_prepare_v2(c->db,
            "UPDATE job SET state='pending', run_at=?1+repeat_every, attempt=0, "
            "claimed_at=NULL, last_error=NULL WHERE id=?2 AND repeat_every>0",
            -1, &st, NULL) != SQLITE_OK) { rc = JOBQ_EIO; goto out; }
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int64(st, 2, id);
    if (sqlite3_step(st) != SQLITE_DONE) { rc = JOBQ_EIO; goto out; }
    if (sqlite3_changes(c->db) == 0) {
        sqlite3_finalize(st); st = NULL;
        if (sqlite3_prepare_v2(c->db, "DELETE FROM job WHERE id=?", -1, &st, NULL) != SQLITE_OK) { rc = JOBQ_EIO; goto out; }
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) != SQLITE_DONE) rc = JOBQ_EIO;
    }
out:
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static int jq_fail(void *vctx, long long id, const char *error, long long retry_at) {
    jq_ctx *c = vctx;
    pthread_mutex_lock(&c->mu);
    sqlite3_stmt *st = NULL;
    int rc = JOBQ_OK;
    /* attempt was bumped at claim; out of attempts → dead, else retry at retry_at. */
    if (sqlite3_prepare_v2(c->db,
            "UPDATE job SET "
            "  state = CASE WHEN attempt >= max_attempts THEN 'dead' ELSE 'pending' END,"
            "  run_at = CASE WHEN attempt >= max_attempts THEN run_at ELSE ?2 END,"
            "  claimed_at = NULL, last_error = ?3 WHERE id = ?1", -1, &st, NULL) != SQLITE_OK) {
        rc = JOBQ_EIO; goto out;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, retry_at);
    error ? sqlite3_bind_text(st, 3, error, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 3);
    if (sqlite3_step(st) != SQLITE_DONE) rc = JOBQ_EIO;
out:
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static long long jq_count(void *vctx, const char *state) {
    jq_ctx *c = vctx;
    pthread_mutex_lock(&c->mu);
    sqlite3_stmt *st = NULL;
    long long n = JOBQ_EIO;
    const char *sql = state ? "SELECT count(*) FROM job WHERE state=?"
                            : "SELECT count(*) FROM job";
    if (sqlite3_prepare_v2(c->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (state) sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    if (st) sqlite3_finalize(st);
    pthread_mutex_unlock(&c->mu);
    return n;
}

static void jq_destroy(void *vctx) {
    jq_ctx *c = vctx;
    if (!c) return;
    pthread_mutex_destroy(&c->mu);
    free(c);
}

void job_free(job_t *job) {
    if (!job) return;
    free(job->type); free(job->payload);
    job->type = job->payload = NULL;
}

int job_queue_sqlite_open(void *db, job_queue_t *out) {
    if (!db || !out) return JOBQ_EINVAL;
    sqlite3 *sdb = (sqlite3 *)db;
    if (sqlite3_exec(sdb, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) return JOBQ_EIO;

    jq_ctx *c = calloc(1, sizeof *c);
    if (!c) return JOBQ_ENOMEM;
    c->db = sdb;
    if (pthread_mutex_init(&c->mu, NULL) != 0) { free(c); return JOBQ_EIO; }

    out->ctx      = c;
    out->enqueue  = jq_enqueue;
    out->claim    = jq_claim;
    out->complete = jq_complete;
    out->fail     = jq_fail;
    out->count    = jq_count;
    out->destroy  = jq_destroy;
    return JOBQ_OK;
}

/* ---- generic worker tick (port-only; one place defines it) -------------- */

/* exponential backoff with a 1h cap: retry_at = now + min(2^attempt, 3600). */
static long long backoff_at(long long now, int attempt) {
    long long d = 1;
    for (int i = 0; i < attempt && d < 3600; i++) d *= 2;
    if (d > 3600) d = 3600;
    return now + d;
}

int jobq_run_due(job_queue_t *q, long long now, int visibility, int budget,
                 job_handler_fn handler, void *user) {
    if (!q || !handler || budget <= 0) return JOBQ_EINVAL;
    int processed = 0;
    for (int i = 0; i < budget; i++) {
        job_t job;
        int rc = q->claim(q->ctx, now, visibility, &job);
        if (rc == JOBQ_NONE) break;
        if (rc != JOBQ_OK) return rc;
        int hr = handler(user, &job);
        if (hr == 0) q->complete(q->ctx, job.id, now);
        else         q->fail(q->ctx, job.id, "handler failed", backoff_at(now, job.attempt));
        job_free(&job);
        processed++;
    }
    return processed;
}
