#!/usr/bin/env python3
"""pgforge load generator (stdlib only — no wrk/hey needed).

Closed-loop HTTP load: `connections` persistent keep-alive connections (spread
across worker processes so the GIL doesn't cap the generator) each fire requests
back-to-back for `duration` seconds, recording per-request latency. Reports
throughput and latency percentiles. It measures what a client observes; for very
high request rates a native tool (wrk) can push harder, but this needs nothing
installed and is reproducible.

  loadtest.py --label "list" --url http://127.0.0.1:8080/api/products \\
              -H "Authorization: Bearer <tok>" --connections 50 --duration 10
"""
import argparse, http.client, multiprocessing as mp, os, time
from urllib.parse import urlparse


def _thread_loop(host, port, method, path, headers, body, deadline, out):
    lat, err, n = [], 0, 0
    conn = http.client.HTTPConnection(host, port, timeout=15)
    while time.monotonic() < deadline:
        t0 = time.perf_counter()
        try:
            conn.request(method, path, body=body, headers=headers)
            r = conn.getresponse(); r.read()
            lat.append(time.perf_counter() - t0)
            n += 1
            if r.status >= 400: err += 1
        except Exception:
            err += 1
            try: conn.close()
            except Exception: pass
            conn = http.client.HTTPConnection(host, port, timeout=15)
    try: conn.close()
    except Exception: pass
    out.append((lat, err, n))


def _proc_worker(a):
    import threading
    host, port, method, path, headers, body, duration, nthreads = a
    deadline = time.monotonic() + duration
    out = []
    lock = threading.Lock()
    sinks = [[] for _ in range(nthreads)]
    ths = [threading.Thread(target=_thread_loop,
                            args=(host, port, method, path, headers, body, deadline, sinks[i]))
           for i in range(nthreads)]
    for t in ths: t.start()
    for t in ths: t.join()
    lat, err, n = [], 0, 0
    for s in sinks:
        for (l, e, c) in s:
            lat += l; err += e; n += c
    return lat, err, n


def _pct(sorted_lat, p):
    if not sorted_lat: return 0.0
    k = (len(sorted_lat) - 1) * p
    f = int(k); c = min(f + 1, len(sorted_lat) - 1)
    return sorted_lat[f] + (sorted_lat[c] - sorted_lat[f]) * (k - f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="bench")
    ap.add_argument("--url", required=True)
    ap.add_argument("--method", default="GET")
    ap.add_argument("-H", "--header", action="append", default=[])
    ap.add_argument("--body", default=None)
    ap.add_argument("--connections", type=int, default=50)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--procs", type=int, default=0)
    args = ap.parse_args()

    u = urlparse(args.url)
    host, port = u.hostname, (u.port or 80)
    path = u.path + (("?" + u.query) if u.query else "")
    headers = {}
    for h in args.header:
        k, _, v = h.partition(":")
        headers[k.strip()] = v.strip()
    if args.body is not None:
        headers.setdefault("Content-Type", "application/json")

    procs = args.procs or min(os.cpu_count() or 4, max(1, args.connections))
    procs = max(1, min(procs, args.connections))
    per = max(1, args.connections // procs)
    conns = procs * per

    work = (host, port, args.method, path, headers, args.body, args.duration, per)
    t0 = time.monotonic()
    with mp.Pool(procs) as pool:
        results = pool.map(_proc_worker, [work] * procs)
    wall = time.monotonic() - t0

    lat, err, n = [], 0, 0
    for (l, e, c) in results:
        lat += l; err += e; n += c
    lat.sort()
    rps = n / wall if wall else 0
    ms = lambda x: x * 1000.0
    p50, p90, p99 = _pct(lat, .50), _pct(lat, .90), _pct(lat, .99)
    mx = lat[-1] if lat else 0
    print(f"  {args.label:<28} {n:>8} reqs  {rps:>9.0f} req/s   "
          f"p50={ms(p50):6.2f}  p90={ms(p90):6.2f}  p99={ms(p99):7.2f}  max={ms(mx):7.2f} ms"
          f"   conns={conns} errors={err}")


if __name__ == "__main__":
    main()
