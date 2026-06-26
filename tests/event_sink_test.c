/* cellar — EventSink SQLite-adapter unit test. */
#include "event_sink.h"

#include <sqlite3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

/* ---- a capturing visitor ---- */
typedef struct {
    int n, stop_after;
    long long ids[2048], ts[2048];
    char type[2048][24], subject[2048][24], props[2048][32];
} cap_t;

static int capture(void *user, long long id, long long ts, const event_t *ev) {
    cap_t *c = user;
    if (c->n < 2048) {
        c->ids[c->n] = id; c->ts[c->n] = ts;
        snprintf(c->type[c->n], 24, "%s", ev->type ? ev->type : "");
        snprintf(c->subject[c->n], 24, "%s", ev->subject_id ? ev->subject_id : "<null>");
        snprintf(c->props[c->n], 32, "%s", ev->props ? ev->props : "<null>");
    }
    c->n++;
    if (c->stop_after && c->n >= c->stop_after) return 1;   /* early stop */
    return 0;
}

/* ---- concurrency ---- */
#define NTHREAD 8
#define NEACH   100
typedef struct { event_sink_t *s; int id; int ok; } worker_arg;
static void *worker(void *vp) {
    worker_arg *a = vp; a->ok = 1;
    char subj[32];
    for (int i = 0; i < NEACH; i++) {
        snprintf(subj, sizeof subj, "t%d-%d", a->id, i);
        event_t ev = { .type = "view", .actor_id = "u", .subject_id = subj, .props = NULL };
        if (a->s->emit(a->s->ctx, &ev) != EVT_OK) a->ok = 0;
    }
    return NULL;
}

int main(void) {
    printf("event_sink (sqlite)\n");

    char tmpl[] = "/tmp/cellar_evt_XXXXXX";
    int fd = mkstemp(tmpl); if (fd >= 0) close(fd);
    sqlite3 *db = NULL;
    if (sqlite3_open(tmpl, &db) != SQLITE_OK) { fprintf(stderr, "open db\n"); return 2; }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    event_sink_t s;
    chk("open sink", event_sink_sqlite_open(db, &s) == EVT_OK);

    /* validation */
    event_t bad = { .type = NULL };
    chk("emit NULL type => EINVAL", s.emit(s.ctx, &bad) == EVT_EINVAL);
    event_t empty = { .type = "" };
    chk("emit empty type => EINVAL", s.emit(s.ctx, &empty) == EVT_EINVAL);

    /* emit a few with varied optional fields */
    event_t e1 = { .type = "listing_viewed", .actor_id = "u1", .subject_id = "L1", .props = "{\"src\":\"feed\"}" };
    event_t e2 = { .type = "search",         .actor_id = NULL, .subject_id = NULL, .props = "{\"q\":\"toyota\"}" };
    event_t e3 = { .type = "favorite",       .actor_id = "u2", .subject_id = "L1", .props = NULL };
    chk("emit e1", s.emit(s.ctx, &e1) == EVT_OK);
    chk("emit e2", s.emit(s.ctx, &e2) == EVT_OK);
    chk("emit e3", s.emit(s.ctx, &e3) == EVT_OK);

    /* read all from cursor 0, ascending */
    cap_t cap = {0};
    long long n = s.read(s.ctx, 0, 100, capture, &cap);
    chk("read returns 3", n == 3 && cap.n == 3);
    chk("ascending ids", cap.n == 3 && cap.ids[0] < cap.ids[1] && cap.ids[1] < cap.ids[2]);
    chk("type preserved", strcmp(cap.type[0], "listing_viewed") == 0);
    chk("subject preserved", strcmp(cap.subject[0], "L1") == 0);
    chk("null subject -> <null>", strcmp(cap.subject[1], "<null>") == 0);
    chk("props preserved", strcmp(cap.props[0], "{\"src\":\"feed\"}") == 0);
    chk("null props -> <null>", strcmp(cap.props[2], "<null>") == 0);
    chk("ts stamped (>0)", cap.ts[0] > 0);

    /* cursor pagination */
    cap_t p1 = {0};
    s.read(s.ctx, 0, 2, capture, &p1);
    chk("page 1 of 2", p1.n == 2);
    cap_t p2 = {0};
    long long after = p1.ids[1];
    s.read(s.ctx, after, 2, capture, &p2);
    chk("page 2 continues from cursor", p2.n == 1 && p2.ids[0] > after);

    /* early stop via visitor */
    cap_t es = { .stop_after = 2 };
    long long got = s.read(s.ctx, 0, 100, capture, &es);
    chk("visitor early-stop", got == 2);

    /* concurrent emit */
    pthread_t th[NTHREAD]; worker_arg args[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) { args[i] = (worker_arg){ .s = &s, .id = i }; pthread_create(&th[i], NULL, worker, &args[i]); }
    int allok = 1;
    for (int i = 0; i < NTHREAD; i++) { pthread_join(th[i], NULL); if (!args[i].ok) allok = 0; }
    chk("concurrent emits ok", allok);

    /* total count = 3 + NTHREAD*NEACH (drain in pages) */
    long long total = 0, cursor = 0;
    for (;;) {
        cap_t c = {0};
        long long r = s.read(s.ctx, cursor, 256, capture, &c);
        if (r <= 0) break;
        total += r; cursor = c.ids[c.n - 1];
    }
    chk("all events durable", total == 3 + NTHREAD * NEACH);

    s.destroy(s.ctx);
    sqlite3_close(db);
    char cmd[256]; snprintf(cmd, sizeof cmd, "rm -f '%s' '%s-wal' '%s-shm'", tmpl, tmpl, tmpl);
    if (system(cmd) != 0) fprintf(stderr, "warn: cleanup\n");

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
