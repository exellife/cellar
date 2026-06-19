/* cellar — metrics registry + Prometheus rendering unit test (no DB). */
#include "metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

/* True if `doc` contains the exact line `needle` (between newlines or at start). */
static int has_line(const char *doc, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = doc; (p = strstr(p, needle)); p += nlen) {
        int at_bol = (p == doc) || (p[-1] == '\n');
        int at_eol = (p[nlen] == '\n') || (p[nlen] == '\0');
        if (at_bol && at_eol) return 1;
    }
    return 0;
}

static void fill_gauges(cel_gauges_t *g) {
    g->db_pool_size           = 8;
    g->db_pool_in_use         = 3;
    g->realtime_subscriptions = 5;
    g->active_connections     = 2;
}

int main(void) {
    printf("metrics registry\n");
    cel_metrics_init("9.9.9");

    /* ---- counters render with the bumped values + labels ---- */
    cel_metric_inc(CEL_M_HTTP_2XX);
    cel_metric_inc(CEL_M_HTTP_2XX);
    cel_metric_inc(CEL_M_HTTP_4XX);
    cel_metric_add(CEL_M_LOGIN_FAIL, 3);
    cel_metric_inc(CEL_M_LOGIN_OK);
    cel_metric_inc(CEL_M_RATELIMITED);
    cel_metric_inc(CEL_M_SCACHE_HIT);
    cel_metric_inc(CEL_M_RT_EVENTS);

    cel_metrics_set_gauges(fill_gauges);

    char *doc = cel_metrics_render();
    chk("render returns a document", doc != NULL);
    if (!doc) { printf("\nFAILED (%d)\n", ++failures); return 1; }

    chk("http 2xx counter", has_line(doc, "cel_http_requests_total{status=\"2xx\"} 2"));
    chk("http 4xx counter", has_line(doc, "cel_http_requests_total{status=\"4xx\"} 1"));
    chk("http 5xx counter (zero)", has_line(doc, "cel_http_requests_total{status=\"5xx\"} 0"));
    chk("login ok counter", has_line(doc, "cel_auth_logins_total{result=\"ok\"} 1"));
    chk("login fail counter (add)", has_line(doc, "cel_auth_logins_total{result=\"fail\"} 3"));
    chk("ratelimited counter", has_line(doc, "cel_auth_ratelimited_total 1"));
    chk("scache hit counter", has_line(doc, "cel_session_cache_lookups_total{result=\"hit\"} 1"));
    chk("realtime events counter", has_line(doc, "cel_realtime_events_total 1"));

    /* ---- gauges come from the provider ---- */
    chk("pool in_use gauge", has_line(doc, "cel_db_pool_connections{state=\"in_use\"} 3"));
    chk("pool total gauge", has_line(doc, "cel_db_pool_connections{state=\"total\"} 8"));
    chk("subscriptions gauge", has_line(doc, "cel_realtime_subscriptions 5"));
    chk("active connections gauge", has_line(doc, "cel_active_connections 2"));

    /* ---- HELP/TYPE metadata + build info ---- */
    chk("TYPE line present", has_line(doc, "# TYPE cel_http_requests_total counter"));
    chk("build_info version label", has_line(doc, "cel_build_info{version=\"9.9.9\"} 1"));

    free(doc);

    /* ---- histogram: three observations land in cumulative buckets ---- */
    cel_metric_http_observe(0.002);   /* (0.001, 0.0025] */
    cel_metric_http_observe(0.03);    /* (0.025, 0.05]  */
    cel_metric_http_observe(100.0);   /* +Inf overflow  */
    doc = cel_metrics_render();
    chk("hist count == 3", has_line(doc, "cel_http_request_duration_seconds_count 3"));
    /* le=0.001 sees nothing; le=0.0025 sees the 0.002 sample (cumulative=1). */
    chk("hist le=0.001 cumulative 0", has_line(doc, "cel_http_request_duration_seconds_bucket{le=\"0.001\"} 0"));
    chk("hist le=0.0025 cumulative 1", has_line(doc, "cel_http_request_duration_seconds_bucket{le=\"0.0025\"} 1"));
    /* le=0.05 has the 0.002 and 0.03 samples (cumulative=2). */
    chk("hist le=0.05 cumulative 2", has_line(doc, "cel_http_request_duration_seconds_bucket{le=\"0.05\"} 2"));
    /* +Inf has all three. */
    chk("hist +Inf cumulative 3", has_line(doc, "cel_http_request_duration_seconds_bucket{le=\"+Inf\"} 3"));
    chk("hist TYPE histogram", has_line(doc, "# TYPE cel_http_request_duration_seconds histogram"));
    free(doc);

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
