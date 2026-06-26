/* ============================================================================
 * cellar — JobQueue: durable background work, behind a port.
 *
 * A generic engine primitive: enqueue a typed job (+ JSON payload), and a worker
 * atomically claims due jobs and runs them, with a visibility timeout (a claim
 * not completed within it becomes reclaimable — crash-safety), retry with
 * backoff, a dead-letter terminal state, scheduling (run_at), and recurring jobs
 * (repeat_every). The engine depends only on this vtable; the default adapter is
 * a SQLite table + worker, swappable later (Redis/NATS) behind the same port.
 *
 * Port-first: no SQL leaks through this header. The queue never learns a domain
 * noun — "expire_listings" is just a caller-chosen `type`; what a job DOES is the
 * caller's handler (in cellar, a Lua `job` hook).
 * ============================================================================ */
#ifndef CEL_JOB_QUEUE_H
#define CEL_JOB_QUEUE_H

#include <stddef.h>

enum {
    JOBQ_OK     =  0,
    JOBQ_EINVAL = -1,
    JOBQ_EIO    = -2,
    JOBQ_ENOMEM = -3,
    JOBQ_NONE   = -4    /* claim found nothing due                          */
};

/* A claimed job. `type`/`payload` are owned by the struct (free with job_free).
 * `attempt` is this run's attempt number (1-based; incremented at claim). */
typedef struct {
    long long id;
    char     *type;
    char     *payload;     /* JSON; may be NULL */
    int        attempt;
    int        max_attempts;
} job_t;

/* A job handler: returns 0 on success, non-zero to fail (→ retry/dead-letter).
 * Used by jobq_run_due. */
typedef int (*job_handler_fn)(void *user, const job_t *job);

typedef struct job_queue {
    void *ctx;

    /* Enqueue a job. run_at = unix secs to first run (0 = now). max_attempts 0 =
     * adapter default. repeat_every = recurring interval secs (0 = one-shot).
     * Writes the new id to *out_id if non-NULL. */
    int (*enqueue)(void *ctx, const char *type, const char *payload,
                   long long run_at, int max_attempts, long long repeat_every,
                   long long *out_id);

    /* Atomically claim the next due job (a pending job with run_at <= now, OR a
     * claim older than `visibility` secs — reclaimed after a crash). Fills *job
     * and returns JOBQ_OK, or JOBQ_NONE if nothing is due, or a negative code. */
    int (*claim)(void *ctx, long long now, int visibility, job_t *job);

    /* Mark a claimed job done: deletes a one-shot job, or re-schedules a
     * recurring one at now + repeat_every. */
    int (*complete)(void *ctx, long long id, long long now);

    /* Record a failure: re-queues at `retry_at` while attempts remain, else moves
     * the job to the dead-letter state. `error` is stored (may be NULL). */
    int (*fail)(void *ctx, long long id, const char *error, long long retry_at);

    /* Count jobs in a state ("pending" | "claimed" | "dead"), or all if NULL. */
    long long (*count)(void *ctx, const char *state);

    void (*destroy)(void *ctx);
} job_queue_t;

/* Free the owned fields of a claimed job (safe on a zeroed job). */
void job_free(job_t *job);

/* One worker tick (no threads): claim up to `budget` due jobs and run `handler`
 * on each — completing on success, failing with exponential backoff on error or
 * exception. Returns the number processed (>= 0) or a negative code. The engine
 * drives this on a timer/thread; tests drive it directly for determinism. */
int jobq_run_due(job_queue_t *q, long long now, int visibility, int budget,
                 job_handler_fn handler, void *user);

/* ---- default adapter: SQLite -------------------------------------------- */
/* Appends to a `job` table in `db` (sqlite3*, created if absent). `db` is
 * caller-owned and must outlive the queue. Thread-safe (internal mutex). */
int job_queue_sqlite_open(void *db, job_queue_t *out);

#endif /* CEL_JOB_QUEUE_H */
