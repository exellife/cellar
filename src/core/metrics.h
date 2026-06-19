/* ============================================================================
 * pgforge — in-process metrics registry (Prometheus text exposition).
 *
 * A leaf module (like rate_limit / session_cache): it depends on nothing else in
 * pgforge, so any subsystem can `#include "metrics.h"` and bump a counter without
 * a dependency cycle. Counters are lock-free atomics (a counter bump is on the hot
 * path); the latency histogram takes a short mutex; live gauges (pool in-use,
 * active subscriptions, WS connections) are *pulled* at scrape time through a
 * provider the host registers — so this module never reaches up into db/realtime.
 *
 * Scrape it at GET /metrics. The series set is fixed and small (no user-controlled
 * labels), so cardinality is bounded by construction.
 * ============================================================================ */
#ifndef PGF_METRICS_H
#define PGF_METRICS_H

/* Monotonic counters. */
typedef enum {
    PGF_M_HTTP_2XX,          /* responses by status class */
    PGF_M_HTTP_4XX,
    PGF_M_HTTP_5XX,
    PGF_M_LOGIN_OK,          /* login attempts by outcome (REST + WS) */
    PGF_M_LOGIN_FAIL,
    PGF_M_RATELIMITED,       /* auth requests rejected with 429 */
    PGF_M_SCACHE_HIT,        /* session-cache lookups (only when cache is enabled) */
    PGF_M_SCACHE_MISS,
    PGF_M_DB_POOL_WAIT,      /* acquires that had to block for a free connection */
    PGF_M_DB_ACQUIRE_FAIL,   /* acquires that returned no connection */
    PGF_M_RT_EVENTS,         /* realtime CHANGE events entering fan-out */
    PGF_M_STMT_PREPARE,      /* server-side PQprepare calls (cache miss → plan) */
    PGF_M_STMT_REUSE,        /* PQexecPrepared on a cached plan (cache hit → no re-plan) */
    PGF_M_DB_PIPELINE,       /* pooled-mode transactions sent as one pipelined round trip */
    PGF_M__COUNT
} pgf_counter_t;

/* Record the process start time and build version (call once at startup). */
void pgf_metrics_init(const char *version);

/* The build version string set at init (or "unknown"). */
const char *pgf_metrics_version(void);

void pgf_metric_inc(pgf_counter_t c);
void pgf_metric_add(pgf_counter_t c, long n);

/* Observe one HTTP request-handling duration (seconds) into the histogram. */
void pgf_metric_http_observe(double seconds);

/* Gauges sampled live at scrape time. The host registers a provider that fills
 * these from the subsystems that own them (keeps this module a leaf). */
typedef struct {
    int  db_pool_size;
    int  db_pool_in_use;
    long realtime_subscriptions;
    long active_connections;     /* live transport connections (HTTP + WS) */
} pgf_gauges_t;
typedef void (*pgf_gauge_provider_fn)(pgf_gauges_t *out);
void pgf_metrics_set_gauges(pgf_gauge_provider_fn fn);

/* Render the whole registry as a Prometheus text-format document. The caller
 * owns the returned heap string (free it); NULL on allocation failure. */
char *pgf_metrics_render(void);

#endif /* PGF_METRICS_H */
