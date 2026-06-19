/* cellar — token-bucket rate limiter unit test (no DB). */
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
    cel_ratelimit_t *rl = cel_ratelimit_create(3, 60);
    chk("created", rl != NULL);
    chk("1st allowed", cel_ratelimit_allow(rl, "1.2.3.4"));
    chk("2nd allowed", cel_ratelimit_allow(rl, "1.2.3.4"));
    chk("3rd allowed", cel_ratelimit_allow(rl, "1.2.3.4"));
    chk("4th denied", !cel_ratelimit_allow(rl, "1.2.3.4"));
    chk("5th denied", !cel_ratelimit_allow(rl, "1.2.3.4"));

    /* ---- a different key has its own bucket ---- */
    chk("other key allowed", cel_ratelimit_allow(rl, "9.9.9.9"));

    /* ---- empty / NULL key always allowed ---- */
    chk("empty key allowed", cel_ratelimit_allow(rl, ""));
    chk("null key allowed", cel_ratelimit_allow(rl, NULL));
    cel_ratelimit_destroy(rl);

    /* ---- disabled: create returns NULL, a NULL limiter always allows ---- */
    cel_ratelimit_t *off = cel_ratelimit_create(0, 60);
    chk("disabled -> NULL limiter", off == NULL);
    int all = 1;
    for (int i = 0; i < 100; i++) all &= cel_ratelimit_allow(off, "x");
    chk("disabled -> always allow", all);

    /* ---- two independent limiters don't share buckets ---- */
    cel_ratelimit_t *a = cel_ratelimit_create(1, 60);
    cel_ratelimit_t *b = cel_ratelimit_create(1, 60);
    chk("limiter A: 1 ok",  cel_ratelimit_allow(a, "same-key"));
    chk("limiter A: 2 deny", !cel_ratelimit_allow(a, "same-key"));
    chk("limiter B independent", cel_ratelimit_allow(b, "same-key"));   /* B's bucket is separate */
    cel_ratelimit_destroy(a); cel_ratelimit_destroy(b);

    /* ---- refill over time: 2 per 1s ---- */
    cel_ratelimit_t *rf = cel_ratelimit_create(2, 1);
    chk("refill: 1 ok", cel_ratelimit_allow(rf, "k"));
    chk("refill: 2 ok", cel_ratelimit_allow(rf, "k"));
    chk("refill: 3 denied", !cel_ratelimit_allow(rf, "k"));
    sleep(2);                                   /* > window: bucket refills */
    chk("refill: allowed again", cel_ratelimit_allow(rf, "k"));
    cel_ratelimit_destroy(rf);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
