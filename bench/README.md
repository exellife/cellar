# pgforge load / perf harness

A self-contained throughput + latency harness — **stdlib Python only, no `wrk`/`hey`
needed**. It boots pgforge with the demo data, then drives closed-loop HTTP load
against the key paths and reports requests/sec and latency percentiles.

```sh
bench/bench.sh ./build/pgforge [duration-seconds] [connections]
# e.g.
bench/bench.sh ./build-cmake/pgforge 10 50
```

It uses `PGF_DB_*` from the environment (defaults: localhost / postgres / pgforge),
seeds a `bench@pgforge.dev` admin, runs with the auth rate limiter **off** (so the
benchmark isn't throttled), and tears the server down afterward.

`loadtest.py` is the generator and can be pointed at any endpoint:

```sh
python3 bench/loadtest.py --label list --url http://127.0.0.1:8080/api/products \
    -H "Authorization: Bearer <token>" --connections 50 --duration 10
```

## What it measures

Four paths, plus a session-cache on/off comparison for the hot authed-read path:

| scenario | what it exercises |
|---|---|
| `health` | pure portico transport — no auth, no DB |
| `list products (authed)` | the hot path: token resolution + a DB read |
| `get product (authed)` | token resolution + single-row read |
| `login (Argon2id)` | the deliberately expensive credential path |
| `list (cached auth)` | same authed read with `PGF_SESSION_CACHE_TTL` set |

## Illustrative numbers (32-core x86_64 dev box, loopback)

```
[session cache OFF]
  health (no auth, no DB)     ~113k req/s   p50 0.17 ms
  list products (authed)      ~8.5k req/s   p50 2.3  ms
  get product (authed)        ~8.5k req/s   p50 2.3  ms
  login (Argon2id)            ~72  req/s    p50 107  ms
[session cache ON, TTL=30]
  list products (cached auth) ~18k req/s    p50 1.1  ms   (~2.1x)
```

How to read them:
- **Transport is not the bottleneck** — health is ~13x the authed-read rate; the cost
  is the database round trip, not portico.
- **The session cache roughly doubles authed-read throughput** by skipping the
  per-request `pgf_sessions ⋈ pgf_users` lookup (set `PGF_SESSION_CACHE_TTL`).
- **Login is meant to be slow** (Argon2id ≈ 100 ms). At ~72 req/s it is a CPU-DoS
  amplifier if left unguarded — which is exactly why `/auth/login` is rate-limited
  (`PGF_AUTH_RATELIMIT`) and accounts can be locked (`PGF_AUTH_LOCKOUT`).

## Caveats

- Numbers are **client-observed** from a Python generator; for very high request
  rates the generator (not the server) can be the limit — a native tool like `wrk`
  pushes harder. The relative comparisons (cache on/off, transport vs. DB) hold either way.
- Loopback removes network latency; real deployments add it.
- This is a **manual tool**, not part of `ctest` (it produces numbers, not pass/fail).
