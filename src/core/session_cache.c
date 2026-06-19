#include "session_cache.h"
#include "metrics.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Chained hash, single mutex. The critical section is a short chain walk + a
 * struct copy, so one lock is fine for v1; shard by bucket if it ever shows up
 * in a profile. Memory is bounded by MAX_ENTRIES with a lazy sweep of expired
 * entries when full. */
#define NBUCKETS    4096
#define MAX_ENTRIES 100000

typedef struct sc_node {
    char token[129];
    cel_user_t user;
    time_t revalidate_at;     /* served only while now < revalidate_at */
    struct sc_node *next;
} sc_node_t;

static sc_node_t      *g_buckets[NBUCKETS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_ttl  = 0;     /* <=0 => disabled */
static long            g_count = 0;

static unsigned bucket_of(const char *t) {
    unsigned h = 2166136261u;          /* FNV-1a */
    for (const char *p = t; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h & (NBUCKETS - 1);
}

void cel_session_cache_init(int ttl_seconds) { g_ttl = ttl_seconds; }

static void free_chain(sc_node_t *n) { while (n) { sc_node_t *nx = n->next; free(n); n = nx; } }

void cel_session_cache_cleanup(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < NBUCKETS; i++) { free_chain(g_buckets[i]); g_buckets[i] = NULL; }
    g_count = 0;
    pthread_mutex_unlock(&g_lock);
}

void cel_session_cache_clear(void) { cel_session_cache_cleanup(); }

bool cel_session_cache_get(const char *token, cel_user_t *user) {
    if (g_ttl <= 0 || !token || !*token) return false;
    time_t now = time(NULL);
    bool hit = false;
    unsigned b = bucket_of(token);
    pthread_mutex_lock(&g_lock);
    for (sc_node_t *n = g_buckets[b]; n; n = n->next) {
        if (!strcmp(n->token, token)) {
            if (now < n->revalidate_at) { *user = n->user; hit = true; }   /* else: stale -> miss */
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    cel_metric_inc(hit ? CEL_M_SCACHE_HIT : CEL_M_SCACHE_MISS);
    return hit;
}

/* Drop expired nodes from a chain; returns the new head. Caller holds the lock. */
static sc_node_t *sweep_chain(sc_node_t *n, time_t now) {
    sc_node_t *head = NULL, **pp = &head;
    while (n) {
        sc_node_t *nx = n->next;
        if (now < n->revalidate_at) { *pp = n; pp = &n->next; n->next = NULL; }
        else { free(n); g_count--; }
        n = nx;
    }
    return head;
}

void cel_session_cache_put(const char *token, const cel_user_t *user) {
    if (g_ttl <= 0 || !token || !*token) return;
    time_t now = time(NULL);
    unsigned b = bucket_of(token);
    pthread_mutex_lock(&g_lock);

    for (sc_node_t *n = g_buckets[b]; n; n = n->next) {           /* refresh existing */
        if (!strcmp(n->token, token)) {
            n->user = *user; n->revalidate_at = now + g_ttl;
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    if (g_count >= MAX_ENTRIES) {                                 /* bound memory */
        for (int i = 0; i < NBUCKETS; i++) g_buckets[i] = sweep_chain(g_buckets[i], now);
        if (g_count >= MAX_ENTRIES) { pthread_mutex_unlock(&g_lock); return; }  /* skip caching */
    }
    sc_node_t *n = malloc(sizeof *n);
    if (n) {
        snprintf(n->token, sizeof n->token, "%s", token);
        n->user = *user; n->revalidate_at = now + g_ttl;
        n->next = g_buckets[b]; g_buckets[b] = n; g_count++;
    }
    pthread_mutex_unlock(&g_lock);
}

void cel_session_cache_evict(const char *token) {
    if (!token || !*token) return;
    unsigned b = bucket_of(token);
    pthread_mutex_lock(&g_lock);
    sc_node_t **pp = &g_buckets[b];
    while (*pp) {
        sc_node_t *n = *pp;
        if (!strcmp(n->token, token)) { *pp = n->next; free(n); g_count--; break; }
        pp = &n->next;
    }
    pthread_mutex_unlock(&g_lock);
}
