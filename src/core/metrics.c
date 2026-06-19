#include "metrics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Counters: lock-free atomics, indexed by pgf_counter_t. */
static atomic_llong g_counters[PGF_M__COUNT];

/* HTTP request-duration histogram. Cumulative Prometheus buckets are derived at
 * render time from these per-bucket tallies; g_hist[NBUCKETS] is the +Inf overflow.
 * Guarded by one mutex — an observe is a couple of adds, not worth atomics. */
static const double g_bounds[] = {
    0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
};
#define NBUCKETS ((int)(sizeof g_bounds / sizeof g_bounds[0]))

static pthread_mutex_t g_hist_lock = PTHREAD_MUTEX_INITIALIZER;
static long            g_hist[NBUCKETS + 1];
static double          g_hist_sum;

static time_t                 g_started;
static char                   g_version[32] = "unknown";
static pgf_gauge_provider_fn  g_gauges;

const char *pgf_metrics_version(void) { return g_version; }

void pgf_metrics_init(const char *version) {
    g_started = time(NULL);
    if (version && *version) snprintf(g_version, sizeof g_version, "%s", version);
}

void pgf_metric_inc(pgf_counter_t c) {
    if (c < PGF_M__COUNT) atomic_fetch_add(&g_counters[c], 1);
}

void pgf_metric_add(pgf_counter_t c, long n) {
    if (c < PGF_M__COUNT) atomic_fetch_add(&g_counters[c], n);
}

void pgf_metric_http_observe(double seconds) {
    if (seconds < 0) seconds = 0;
    int i = 0;
    while (i < NBUCKETS && seconds > g_bounds[i]) i++;   /* first bucket with secs <= le */
    pthread_mutex_lock(&g_hist_lock);
    g_hist[i]++;
    g_hist_sum += seconds;
    pthread_mutex_unlock(&g_hist_lock);
}

void pgf_metrics_set_gauges(pgf_gauge_provider_fn fn) { g_gauges = fn; }

/* ---- rendering ------------------------------------------------------------- */

static long counter(pgf_counter_t c) { return (long)atomic_load(&g_counters[c]); }

char *pgf_metrics_render(void) {
    /* Snapshot the histogram under the lock, then render lock-free. */
    long   hist[NBUCKETS + 1];
    double hsum;
    pthread_mutex_lock(&g_hist_lock);
    memcpy(hist, g_hist, sizeof hist);
    hsum = g_hist_sum;
    pthread_mutex_unlock(&g_hist_lock);

    pgf_gauges_t g = {0};
    if (g_gauges) g_gauges(&g);
    long uptime = g_started ? (long)(time(NULL) - g_started) : 0;

    char  *buf = NULL;
    size_t cap = 0;
    FILE  *f = open_memstream(&buf, &cap);
    if (!f) return NULL;

    fputs("# HELP pgf_http_requests_total HTTP responses handled, by status class.\n"
          "# TYPE pgf_http_requests_total counter\n", f);
    fprintf(f, "pgf_http_requests_total{status=\"2xx\"} %ld\n", counter(PGF_M_HTTP_2XX));
    fprintf(f, "pgf_http_requests_total{status=\"4xx\"} %ld\n", counter(PGF_M_HTTP_4XX));
    fprintf(f, "pgf_http_requests_total{status=\"5xx\"} %ld\n", counter(PGF_M_HTTP_5XX));

    fputs("# HELP pgf_auth_logins_total Login attempts by outcome.\n"
          "# TYPE pgf_auth_logins_total counter\n", f);
    fprintf(f, "pgf_auth_logins_total{result=\"ok\"} %ld\n",   counter(PGF_M_LOGIN_OK));
    fprintf(f, "pgf_auth_logins_total{result=\"fail\"} %ld\n", counter(PGF_M_LOGIN_FAIL));

    fputs("# HELP pgf_auth_ratelimited_total Auth requests rejected by the rate limiter (429).\n"
          "# TYPE pgf_auth_ratelimited_total counter\n", f);
    fprintf(f, "pgf_auth_ratelimited_total %ld\n", counter(PGF_M_RATELIMITED));

    fputs("# HELP pgf_session_cache_lookups_total Session-cache lookups by result (cache enabled only).\n"
          "# TYPE pgf_session_cache_lookups_total counter\n", f);
    fprintf(f, "pgf_session_cache_lookups_total{result=\"hit\"} %ld\n",  counter(PGF_M_SCACHE_HIT));
    fprintf(f, "pgf_session_cache_lookups_total{result=\"miss\"} %ld\n", counter(PGF_M_SCACHE_MISS));

    fputs("# HELP pgf_db_pool_waits_total Connection acquires that had to wait for a free slot.\n"
          "# TYPE pgf_db_pool_waits_total counter\n", f);
    fprintf(f, "pgf_db_pool_waits_total %ld\n", counter(PGF_M_DB_POOL_WAIT));

    fputs("# HELP pgf_db_acquire_failures_total Connection acquires that returned no connection.\n"
          "# TYPE pgf_db_acquire_failures_total counter\n", f);
    fprintf(f, "pgf_db_acquire_failures_total %ld\n", counter(PGF_M_DB_ACQUIRE_FAIL));

    fputs("# HELP pgf_realtime_events_total Realtime CHANGE events entering fan-out.\n"
          "# TYPE pgf_realtime_events_total counter\n", f);
    fprintf(f, "pgf_realtime_events_total %ld\n", counter(PGF_M_RT_EVENTS));

    fputs("# HELP pgf_db_prepared_statements_total Per-connection prepared-statement cache, by outcome.\n"
          "# TYPE pgf_db_prepared_statements_total counter\n", f);
    fprintf(f, "pgf_db_prepared_statements_total{result=\"prepare\"} %ld\n", counter(PGF_M_STMT_PREPARE));
    fprintf(f, "pgf_db_prepared_statements_total{result=\"reuse\"} %ld\n",   counter(PGF_M_STMT_REUSE));

    fputs("# HELP pgf_db_pipelined_txns_total Pooled-mode transactions sent as one pipelined round trip.\n"
          "# TYPE pgf_db_pipelined_txns_total counter\n", f);
    fprintf(f, "pgf_db_pipelined_txns_total %ld\n", counter(PGF_M_DB_PIPELINE));

    fputs("# HELP pgf_db_pool_connections Database connection pool, by state.\n"
          "# TYPE pgf_db_pool_connections gauge\n", f);
    fprintf(f, "pgf_db_pool_connections{state=\"in_use\"} %d\n", g.db_pool_in_use);
    fprintf(f, "pgf_db_pool_connections{state=\"total\"} %d\n",  g.db_pool_size);

    fputs("# HELP pgf_realtime_subscriptions Active realtime subscriptions.\n"
          "# TYPE pgf_realtime_subscriptions gauge\n", f);
    fprintf(f, "pgf_realtime_subscriptions %ld\n", g.realtime_subscriptions);

    fputs("# HELP pgf_active_connections Live transport connections (HTTP keep-alive + WebSocket).\n"
          "# TYPE pgf_active_connections gauge\n", f);
    fprintf(f, "pgf_active_connections %ld\n", g.active_connections);

    fputs("# HELP pgf_uptime_seconds Seconds since process start.\n"
          "# TYPE pgf_uptime_seconds gauge\n", f);
    fprintf(f, "pgf_uptime_seconds %ld\n", uptime);

    fputs("# HELP pgf_build_info Build information; the value is always 1.\n"
          "# TYPE pgf_build_info gauge\n", f);
    fprintf(f, "pgf_build_info{version=\"%s\"} 1\n", g_version);

    fputs("# HELP pgf_http_request_duration_seconds HTTP request handling time.\n"
          "# TYPE pgf_http_request_duration_seconds histogram\n", f);
    long cumulative = 0;
    for (int i = 0; i < NBUCKETS; i++) {
        cumulative += hist[i];
        fprintf(f, "pgf_http_request_duration_seconds_bucket{le=\"%g\"} %ld\n",
                g_bounds[i], cumulative);
    }
    cumulative += hist[NBUCKETS];   /* +Inf overflow */
    fprintf(f, "pgf_http_request_duration_seconds_bucket{le=\"+Inf\"} %ld\n", cumulative);
    fprintf(f, "pgf_http_request_duration_seconds_sum %g\n", hsum);
    fprintf(f, "pgf_http_request_duration_seconds_count %ld\n", cumulative);

    fclose(f);
    return buf;
}
