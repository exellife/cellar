#include "rate_limit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Chained hash of per-key token buckets + a mutex, per limiter instance (the
 * critical section is a short chain walk + a few flops). Memory is bounded with a
 * lazy sweep of idle (full) buckets when full — an idle key has refilled to
 * capacity, so dropping it loses no state. Mirrors session_cache.c's structure. */
#define NBUCKETS    1024
#define MAX_ENTRIES 100000
#define KEYLEN      64

typedef struct rl_node {
    char   key[KEYLEN];
    double tokens;
    time_t last;          /* last refill (seconds) */
    struct rl_node *next;
} rl_node_t;

struct cel_ratelimit {
    rl_node_t      *buckets[NBUCKETS];
    pthread_mutex_t lock;
    int             limit;    /* burst size */
    double          rate;     /* tokens per second = limit/window */
    long            count;    /* live bucket count (for the sweep bound) */
};

static unsigned bucket_of(const char *k) {
    unsigned h = 2166136261u;             /* FNV-1a */
    for (const char *p = k; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h & (NBUCKETS - 1);
}

cel_ratelimit_t *cel_ratelimit_create(int limit, int window_seconds) {
    if (limit <= 0 || window_seconds <= 0) return NULL;     /* disabled */
    cel_ratelimit_t *rl = calloc(1, sizeof *rl);
    if (!rl) return NULL;                                   /* fail open */
    rl->limit = limit;
    rl->rate  = (double)limit / (double)window_seconds;
    pthread_mutex_init(&rl->lock, NULL);
    return rl;
}

static void free_chain(rl_node_t *n) { while (n) { rl_node_t *nx = n->next; free(n); n = nx; } }

void cel_ratelimit_destroy(cel_ratelimit_t *rl) {
    if (!rl) return;
    pthread_mutex_lock(&rl->lock);
    for (int i = 0; i < NBUCKETS; i++) { free_chain(rl->buckets[i]); rl->buckets[i] = NULL; }
    pthread_mutex_unlock(&rl->lock);
    pthread_mutex_destroy(&rl->lock);
    free(rl);
}

/* Drop idle (refilled-to-full) buckets from a chain. Caller holds the lock. */
static rl_node_t *sweep_chain(cel_ratelimit_t *rl, rl_node_t *n) {
    rl_node_t *head = NULL, **pp = &head;
    while (n) {
        rl_node_t *nx = n->next;
        if (n->tokens >= (double)rl->limit) { free(n); rl->count--; }   /* full == idle */
        else { *pp = n; pp = &n->next; n->next = NULL; }
        n = nx;
    }
    return head;
}

bool cel_ratelimit_allow(cel_ratelimit_t *rl, const char *key) {
    if (!rl || !key || !*key) return true;                /* disabled / no key */
    time_t now = time(NULL);
    unsigned b = bucket_of(key);
    bool allow;

    pthread_mutex_lock(&rl->lock);
    rl_node_t *n = rl->buckets[b];
    while (n && strcmp(n->key, key) != 0) n = n->next;

    if (!n) {                                     /* new key: full bucket */
        if (rl->count >= MAX_ENTRIES) {
            for (int i = 0; i < NBUCKETS; i++) rl->buckets[i] = sweep_chain(rl, rl->buckets[i]);
        }
        n = malloc(sizeof *n);
        if (!n) { pthread_mutex_unlock(&rl->lock); return true; }  /* fail open */
        snprintf(n->key, sizeof n->key, "%s", key);
        n->tokens = (double)rl->limit;
        n->last = now;
        n->next = rl->buckets[b]; rl->buckets[b] = n; rl->count++;
    } else {                                      /* refill by elapsed time */
        double refill = (double)(now - n->last) * rl->rate;
        if (refill > 0) {
            n->tokens += refill;
            if (n->tokens > (double)rl->limit) n->tokens = (double)rl->limit;
            n->last = now;
        }
    }

    allow = n->tokens >= 1.0;
    if (allow) n->tokens -= 1.0;
    pthread_mutex_unlock(&rl->lock);
    return allow;
}
