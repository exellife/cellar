#!/usr/bin/env python3
"""pgforge observability end-to-end (#Phase 9): GET /metrics is a live Prometheus
scrape and the counters actually move with traffic.

Scrape once for a baseline, drive a little traffic (health checks + one bad login),
scrape again, and assert the relevant series advanced: HTTP 2xx, the request-latency
histogram count, and the failed-login counter. Also checks the exposition basics
(status, content-type, HELP/TYPE metadata, gauges present). Boots via the harness,
which passes a ws://host:port/ URL as argv[1].
"""
import http.client, json, os, sys
from urllib.parse import urlparse

HOST = PORT = None
TOKEN = os.environ.get("PGF_METRICS_TOKEN", "")   # /metrics is bearer-gated (L-3)


def conn():
    return http.client.HTTPConnection(HOST, PORT, timeout=10)


def get(path, token=None):
    c = conn()
    headers = {"Authorization": "Bearer " + token} if token else {}
    c.request("GET", path, headers=headers)
    r = c.getresponse(); body = r.read().decode(); ct = r.getheader("Content-Type") or ""
    c.close()
    return r.status, ct, body


def post(path, obj):
    c = conn()
    c.request("POST", path, body=json.dumps(obj), headers={"Content-Type": "application/json"})
    r = c.getresponse(); r.read(); c.close()
    return r.status


def metric(doc, line_prefix):
    """Value of the series whose line starts with `line_prefix` (the full name incl.
    any {labels}), else None."""
    for line in doc.splitlines():
        if line.startswith(line_prefix) and not line.startswith("#"):
            rest = line[len(line_prefix):]
            if rest[:1] in ("", " "):   # exact name match, not a prefix collision
                return float(rest.strip())
    return None


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<40} {detail}")
        ok += bool(cond); fail += (not cond)

    # ---- L-3: /metrics is bearer-gated (not open by default) ----
    st_no, _, _ = get("/metrics")                          # no Authorization header
    chk("/metrics without token -> 401", st_no == 401, f"status={st_no}")
    st_bad, _, _ = get("/metrics", token="not-the-token")
    chk("/metrics wrong token -> 401", st_bad == 401, f"status={st_bad}")

    # ---- baseline scrape (with the configured token) ----
    st, ct, doc = get("/metrics", token=TOKEN)
    chk("/metrics 200", st == 200, f"status={st}")
    chk("content-type is prometheus text", ct.startswith("text/plain"), ct)
    chk("has TYPE metadata", "# TYPE pgf_http_requests_total counter" in doc)
    chk("has build_info", "pgf_build_info{version=" in doc)
    chk("has uptime gauge", metric(doc, "pgf_uptime_seconds") is not None)
    chk("has active connections gauge", metric(doc, "pgf_active_connections") is not None)
    chk("active connections non-negative", (metric(doc, "pgf_active_connections") or 0) >= 0)
    chk("has db pool gauge", 'pgf_db_pool_connections{state="total"}' in doc)

    base_2xx  = metric(doc, 'pgf_http_requests_total{status="2xx"}') or 0
    base_hist = metric(doc, "pgf_http_request_duration_seconds_count") or 0
    base_fail = metric(doc, 'pgf_auth_logins_total{result="fail"}') or 0

    # ---- drive traffic ----
    for _ in range(5):
        get("/health")
    bad = post("/auth/login", {"email": "nobody@pgforge.dev", "password": "wrong-pw"})
    chk("bad login rejected", bad in (400, 401), f"status={bad}")

    # ---- scrape again; the counters moved ----
    st, _, doc2 = get("/metrics", token=TOKEN)
    chk("/metrics 200 (again)", st == 200)
    new_2xx  = metric(doc2, 'pgf_http_requests_total{status="2xx"}') or 0
    new_hist = metric(doc2, "pgf_http_request_duration_seconds_count") or 0
    new_fail = metric(doc2, 'pgf_auth_logins_total{result="fail"}') or 0

    # 5 /health + the two /metrics scrapes are all 2xx -> at least +6 since baseline.
    chk("2xx counter advanced", new_2xx >= base_2xx + 6, f"{base_2xx} -> {new_2xx}")
    chk("latency histogram advanced", new_hist >= base_hist + 6, f"{base_hist} -> {new_hist}")
    chk("failed-login counter advanced", new_fail >= base_fail + 1, f"{base_fail} -> {new_fail}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
