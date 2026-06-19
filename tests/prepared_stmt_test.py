#!/usr/bin/env python3
"""pgforge prepared-statement cache (perf lever): the data-API path
(run_rows → db_connection_exec_cached) PQprepares each distinct SQL once per
pooled connection and PQexecPrepares thereafter, so repeated query shapes stop
re-planning. Proven observably via the pgf_db_prepared_statements_total counter:
hammer one identical query shape and assert "reuse" dominates "prepare".

Negative control: boot the server with PGF_PREPARED_STATEMENTS=0 and re-run —
exec_cached falls back to PQexecParams, both counters stay flat, and the
"reuse advanced" assertion below fails (proving the test exercises the feature).

Boots via run_with_server.py, which passes ws://host:port/ as argv[1].
"""
import http.client, json, os, sys
from urllib.parse import urlparse

HOST = PORT = None
TOKEN = os.environ.get("PGF_METRICS_TOKEN", "")   # /metrics is bearer-gated (L-3)
ADMIN = ("admin@pgforge.dev", "s3cret-admin")
N = 120


def conn():
    return http.client.HTTPConnection(HOST, PORT, timeout=10)


def get(path, token=None):
    c = conn()
    h = {"Authorization": "Bearer " + token} if token else {}
    c.request("GET", path, headers=h)
    r = c.getresponse(); body = r.read().decode(); c.close()
    return r.status, body


def login():
    c = conn()
    c.request("POST", "/auth/login",
              body=json.dumps({"email": ADMIN[0], "password": ADMIN[1]}),
              headers={"Content-Type": "application/json"})
    r = c.getresponse(); b = json.loads(r.read()); c.close()
    return b.get("token")


def metric(doc, line_prefix):
    for line in doc.splitlines():
        if line.startswith(line_prefix) and not line.startswith("#"):
            rest = line[len(line_prefix):]
            if rest[:1] in ("", " "):
                return float(rest.strip())
    return None


def counters(token):
    _, doc = get("/metrics", token=TOKEN)
    return (metric(doc, 'pgf_db_prepared_statements_total{result="prepare"}') or 0,
            metric(doc, 'pgf_db_prepared_statements_total{result="reuse"}') or 0)


def main():
    ok = 0; fail = 0
    def chk(name, cond, detail=""):
        nonlocal ok, fail
        print(f"  {'ok' if cond else 'FAIL':<5} {name:<42} {detail}")
        ok += bool(cond); fail += (not cond)

    print(f"== pgforge prepared-statement cache -> {HOST}:{PORT} ==")

    token = login()
    chk("admin login", bool(token))

    chk("counter series exposed",
        "pgf_db_prepared_statements_total" in get("/metrics", token=TOKEN)[1])

    prep0, reuse0 = counters(token)

    # Hammer one identical query shape: GET /api/products with no params builds
    # byte-identical SQL every time, so after the first prepare per connection it
    # must reuse.
    codes = set()
    for _ in range(N):
        st, _ = get("/api/products", token=token)
        codes.add(st)
    chk("all list requests 200", codes == {200}, str(sorted(codes)))

    prep1, reuse1 = counters(token)
    dprep, dreuse = prep1 - prep0, reuse1 - reuse0
    print(f"  .. prepare {prep0:.0f}->{prep1:.0f} (+{dprep:.0f}), "
          f"reuse {reuse0:.0f}->{reuse1:.0f} (+{dreuse:.0f}) over {N} requests")

    # Each pooled connection prepares the shape at most once, so prepares are
    # bounded by the pool (default 8); the rest of the N requests reuse the plan.
    chk("reuse advanced (cache hit path)", dreuse >= 80, f"+{dreuse:.0f}")
    chk("prepares bounded by pool", dprep <= 16, f"+{dprep:.0f}")
    chk("reuse dominates prepare", dreuse > dprep * 4, f"reuse +{dreuse:.0f} vs prepare +{dprep:.0f}")

    print(f"\n{'PASS' if fail == 0 else 'FAIL'}  ({ok} ok, {fail} failed)")
    return 1 if fail else 0


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    HOST, PORT = u.hostname or "127.0.0.1", u.port or 8080
    sys.exit(main())
