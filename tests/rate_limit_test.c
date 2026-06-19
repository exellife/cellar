/* pgforge — token-bucket rate limiter unit test (no DB). */
#include "rate_limit.h"

#include <stdio.h>
#include <unistd.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

int main(void) {
    printf("rate limiter\n");

    /* ---- burst of `limit`, then deny ---- */
    pgf_ratelimit_t *rl = pgf_ratelimit_create(3, 60);
    chk("created", rl != NULL);
    chk("1st allowed", pgf_ratelimit_allow(rl, "1.2.3.4"));
    chk("2nd allowed", pgf_ratelimit_allow(rl, "1.2.3.4"));
    chk("3rd allowed", pgf_ratelimit_allow(rl, "1.2.3.4"));
    chk("4th denied", !pgf_ratelimit_allow(rl, "1.2.3.4"));
    chk("5th denied", !pgf_ratelimit_allow(rl, "1.2.3.4"));

    /* ---- a different key has its own bucket ---- */
    chk("other key allowed", pgf_ratelimit_allow(rl, "9.9.9.9"));

    /* ---- empty / NULL key always allowed ---- */
    chk("empty key allowed", pgf_ratelimit_allow(rl, ""));
    chk("null key allowed", pgf_ratelimit_allow(rl, NULL));
    pgf_ratelimit_destroy(rl);

    /* ---- disabled: create returns NULL, a NULL limiter always allows ---- */
    pgf_ratelimit_t *off = pgf_ratelimit_create(0, 60);
    chk("disabled -> NULL limiter", off == NULL);
    int all = 1;
    for (int i = 0; i < 100; i++) all &= pgf_ratelimit_allow(off, "x");
    chk("disabled -> always allow", all);

    /* ---- two independent limiters don't share buckets ---- */
    pgf_ratelimit_t *a = pgf_ratelimit_create(1, 60);
    pgf_ratelimit_t *b = pgf_ratelimit_create(1, 60);
    chk("limiter A: 1 ok",  pgf_ratelimit_allow(a, "same-key"));
    chk("limiter A: 2 deny", !pgf_ratelimit_allow(a, "same-key"));
    chk("limiter B independent", pgf_ratelimit_allow(b, "same-key"));   /* B's bucket is separate */
    pgf_ratelimit_destroy(a); pgf_ratelimit_destroy(b);

    /* ---- refill over time: 2 per 1s ---- */
    pgf_ratelimit_t *rf = pgf_ratelimit_create(2, 1);
    chk("refill: 1 ok", pgf_ratelimit_allow(rf, "k"));
    chk("refill: 2 ok", pgf_ratelimit_allow(rf, "k"));
    chk("refill: 3 denied", !pgf_ratelimit_allow(rf, "k"));
    sleep(2);                                   /* > window: bucket refills */
    chk("refill: allowed again", pgf_ratelimit_allow(rf, "k"));
    pgf_ratelimit_destroy(rf);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
