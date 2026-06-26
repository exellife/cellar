/* cellar — JobQueue SQLite-adapter unit test (fake clock; no real sleeps). */
#include "job_queue.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

/* handler that succeeds unless the payload says "boom" */
static int handler(void *user, const job_t *job) {
    int *calls = user; (*calls)++;
    return (job->payload && strstr(job->payload, "boom")) ? -1 : 0;
}

/* ---- concurrency: claim must never hand the same job to two workers ---- */
#define NJOBS   500
#define NTHREAD 8
static _Atomic int g_claimed[NJOBS + 1];
typedef struct { job_queue_t *q; int dup; } cc_arg;
static void *cc_worker(void *vp) {
    cc_arg *a = vp;
    for (;;) {
        job_t job;
        int rc = a->q->claim(a->q->ctx, 100000, 3600, &job);   /* high vis: stays claimed */
        if (rc == JOBQ_NONE) break;
        if (rc != JOBQ_OK) { a->dup = 1; break; }
        if (atomic_fetch_add(&g_claimed[job.id], 1) != 0) a->dup = 1;   /* double-claim! */
        job_free(&job);
    }
    return NULL;
}

int main(void) {
    printf("job_queue (sqlite)\n");

    char tmpl[] = "/tmp/cellar_jobq_XXXXXX";
    int fd = mkstemp(tmpl); if (fd >= 0) close(fd);
    sqlite3 *db = NULL;
    if (sqlite3_open(tmpl, &db) != SQLITE_OK) { fprintf(stderr, "open db\n"); return 2; }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    job_queue_t q;
    chk("open queue", job_queue_sqlite_open(db, &q) == JOBQ_OK);

    job_t j;
    /* empty → NONE */
    chk("claim empty => NONE", q.claim(q.ctx, 1000, 60, &j) == JOBQ_NONE);

    /* immediate enqueue + claim */
    long long id = 0;
    chk("enqueue", q.enqueue(q.ctx, "ping", "{\"x\":1}", 0, 0, 0, &id) == JOBQ_OK && id > 0);
    chk("claim due", q.claim(q.ctx, 1000, 60, &j) == JOBQ_OK && j.id == id);
    chk("claimed fields", strcmp(j.type, "ping") == 0 && strcmp(j.payload, "{\"x\":1}") == 0 && j.attempt == 1);
    job_free(&j);
    /* claimed (not completed) → not re-claimable within visibility */
    chk("not reclaimable in window", q.claim(q.ctx, 1030, 60, &j) == JOBQ_NONE);
    /* …reclaimable once visibility expires (crash-safety) */
    chk("reclaim after visibility", q.claim(q.ctx, 1100, 60, &j) == JOBQ_OK && j.id == id && j.attempt == 2);
    chk("complete one-shot", q.complete(q.ctx, j.id, 1100) == JOBQ_OK);
    job_free(&j);
    chk("one-shot gone", q.count(q.ctx, NULL) == 0);

    /* scheduled (future) not due yet */
    q.enqueue(q.ctx, "later", NULL, 5000, 0, 0, NULL);
    chk("future not due", q.claim(q.ctx, 1000, 60, &j) == JOBQ_NONE);
    chk("due at run_at", q.claim(q.ctx, 5000, 60, &j) == JOBQ_OK);
    q.complete(q.ctx, j.id, 5000); job_free(&j);

    /* recurring: completes → re-scheduled at now + repeat_every */
    long long rid = 0;
    q.enqueue(q.ctx, "sweep", NULL, 0, 0, 3600, &rid);
    chk("recurring claim", q.claim(q.ctx, 1000, 60, &j) == JOBQ_OK && j.id == rid);
    chk("recurring complete", q.complete(q.ctx, j.id, 1000) == JOBQ_OK); job_free(&j);
    chk("recurring still present", q.count(q.ctx, NULL) == 1);
    chk("recurring not due now", q.claim(q.ctx, 1000, 60, &j) == JOBQ_NONE);
    chk("recurring due after interval", q.claim(q.ctx, 4600, 60, &j) == JOBQ_OK && j.id == rid);
    q.complete(q.ctx, j.id, 4600); job_free(&j);
    /* a recurring job re-schedules forever, so clear the table to reset state */
    sqlite3_exec(db, "DELETE FROM job", NULL, NULL, NULL);

    /* fail → retry until max_attempts → dead-letter */
    long long fid = 0;
    q.enqueue(q.ctx, "flaky", NULL, 0, 2, 0, &fid);     /* max_attempts = 2 */
    chk("claim flaky #1", q.claim(q.ctx, 2000, 60, &j) == JOBQ_OK && j.attempt == 1);
    q.fail(q.ctx, j.id, "boom", 2100); job_free(&j);
    chk("retry pending after fail", q.count(q.ctx, "pending") == 1 && q.count(q.ctx, "dead") == 0);
    chk("not due before retry_at", q.claim(q.ctx, 2050, 60, &j) == JOBQ_NONE);
    chk("claim flaky #2", q.claim(q.ctx, 2100, 60, &j) == JOBQ_OK && j.attempt == 2);
    q.fail(q.ctx, j.id, "boom again", 2200); job_free(&j);
    chk("dead-lettered after max", q.count(q.ctx, "dead") == 1 && q.count(q.ctx, "pending") == 0);
    chk("dead not claimable", q.claim(q.ctx, 9999, 60, &j) == JOBQ_NONE);
    sqlite3_exec(db, "DELETE FROM job", NULL, NULL, NULL);

    /* jobq_run_due tick helper: success completes, failure retries */
    int calls = 0;
    q.enqueue(q.ctx, "ok",   "{}",            0, 0, 0, NULL);
    q.enqueue(q.ctx, "bad",  "{\"x\":\"boom\"}", 0, 0, 0, NULL);
    int processed = jobq_run_due(&q, 3000, 60, 10, handler, &calls);
    chk("tick processed 2", processed == 2 && calls == 2);
    chk("ok job completed (gone)", q.count(q.ctx, NULL) == 1);          /* only the failed one remains */
    chk("bad job retrying (pending)", q.count(q.ctx, "pending") == 1);
    /* reset table AND the autoincrement counter so ids restart at 1 (the test
     * indexes g_claimed[] by job id) */
    sqlite3_exec(db, "DELETE FROM job; DELETE FROM sqlite_sequence WHERE name='job'", NULL, NULL, NULL);

    /* concurrent claim: no double-claim */
    for (int i = 0; i <= NJOBS; i++) atomic_store(&g_claimed[i], 0);
    for (int i = 0; i < NJOBS; i++) q.enqueue(q.ctx, "cc", NULL, 0, 0, 0, NULL);
    pthread_t th[NTHREAD]; cc_arg args[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) { args[i] = (cc_arg){ .q = &q, .dup = 0 }; pthread_create(&th[i], NULL, cc_worker, &args[i]); }
    int dup = 0; for (int i = 0; i < NTHREAD; i++) { pthread_join(th[i], NULL); dup |= args[i].dup; }
    chk("no double-claim under concurrency", !dup);
    int total = 0, each_once = 1;
    for (int i = 1; i <= NJOBS; i++) { int v = atomic_load(&g_claimed[i]); total += v; if (v != 1) each_once = 0; }
    chk("every job claimed exactly once", total == NJOBS && each_once);

    q.destroy(q.ctx);
    sqlite3_close(db);
    char cmd[256]; snprintf(cmd, sizeof cmd, "rm -f '%s' '%s-wal' '%s-shm'", tmpl, tmpl, tmpl);
    if (system(cmd) != 0) fprintf(stderr, "warn: cleanup\n");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
