# cellar load / perf harness

A self-contained throughput + latency harness — **stdlib Python only, no `wrk`/`hey`
needed**. It boots cellar with the demo data, then drives closed-loop HTTP load
against the key paths and reports requests/sec and latency percentiles.

```sh
bench/bench.sh ./build/cellar [duration-seconds] [connections]
# e.g.
bench/bench.sh ./build-cmake/cellar 10 50
```

It seeds a throwaway per-app SQLite database with a demo catalog (`BENCH_PRODUCTS`
rows, default 5000), boots single-app cellar with a `bench@cellar.dev` admin and
the rate limiters **off** (so the benchmark isn't throttled), and tears it all down
afterward. No Postgres, no external setup.

`loadtest.py` is the generator and can be pointed at any endpoint:

```sh
python3 bench/loadtest.py --label list --url http://127.0.0.1:8080/api/products \
    -H "Authorization: Bearer <token>" --connections 50 --duration 10
```

## What it measures

Read + write paths, plus a session-cache on/off comparison for the hot read:

| scenario | what it exercises |
|---|---|
| `health` | pure portico transport — no auth, no DB |
| `list products (authed)` | the hot read path: token resolution + a SQLite read |
| `get product (authed)` | token resolution + single-row read |
| `create product (write)` | the write path: per-request transaction + per-app write lock |
| `login (Argon2id)` | the deliberately expensive credential path |
| `list (cached auth)` | same authed read with `CEL_SESSION_CACHE_TTL` set |

## Illustrative numbers (32-core x86_64, loopback, 2000 products, 20 conns)

```
[session cache OFF]
  health (no auth, no DB)     ~120k req/s   p50 0.16 ms
  list products (authed)      ~44k  req/s   p50 0.45 ms
  get product (authed)        ~55k  req/s   p50 0.35 ms
  create product (write)      ~11k  req/s   p50 1.44 ms  p99 8.3 ms
  login (Argon2id)            ~80   req/s   p50 96   ms
[session cache ON, TTL=30]
  list products (cached auth) ~55k  req/s   p50 0.36 ms
```

How to read them:
- **Reads are fast** — a SQLite read is an in-process lookup, not a network round
  trip, so authed reads are ~5x what the old Postgres backend managed (~8.5k/s).
  Transport (health) is still ~3x the read rate, so the DB isn't the wall either.
- **Writes are single-writer per app** — `create` runs inside a per-request
  transaction under the per-app write lock, so it caps lower than reads and its
  tail (p99) grows with contention. Throughput scales across *distinct apps*, not
  within one (the SQLite-file-per-app tradeoff). Different apps never block each other.
- **The session cache helps reads less here than on Postgres** (~1.25x vs ~2x): the
  per-request `cel_sessions ⋈ cel_users` lookup it skips is a cheap local query, not
  a network hop.
- **Login is meant to be slow** (Argon2id ≈ 100 ms). At ~80 req/s it's a CPU-DoS
  amplifier if unguarded — why `/auth/login` is rate-limited (`CEL_AUTH_RATELIMIT`)
  and accounts can be locked (`CEL_AUTH_LOCKOUT`).

## Caveats

- Numbers are **client-observed** from a Python generator; for very high request
  rates the generator (not the server) can be the limit — a native tool like `wrk`
  pushes harder. The relative comparisons (cache on/off, transport vs. DB) hold either way.
- Loopback removes network latency; real deployments add it.
- This is a **manual tool**, not part of `ctest` (it produces numbers, not pass/fail).
