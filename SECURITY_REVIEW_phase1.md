# Cellar Security Review — Phase 1

Audit of the C backend engine `cellar` (one process hosts many apps; each app is one
SQLite file; isolation is the file boundary) following the Postgres→per-app-SQLite pivot.
Every finding below was adversarially verified against the actual source on a reachable
code path. Line numbers reference the current tree.

## Executive Summary

| Severity | Count |
|----------|-------|
| Critical | 2     |
| High     | 2     |
| Medium   | 1     |
| Low      | 2     |
| **Total**| **7** |

The audit produced 10 raw findings; after merging duplicates of the same root cause
(four separate reports of the cross-registry app_db use-after-free, plus two
concurrency variants of the same "eviction without a pin" defect) the result is 7
distinct issues.

**Top risks**

1. **Cross-registry use-after-free of `app_db_t` (CRITICAL).** Two registries with
   mismatched lifetimes and caps (`cel_apps` caches up to 1024 app handles forever;
   `app_db` LRU-evicts and frees at 64). Any multi-app deployment touching more than 64
   distinct Host bundles will operate on freed memory — remotely driveable by varying the
   Host header. This is the single most dangerous defect and is reported four times.

2. **Realtime publish fans out across apps (CRITICAL).** The realtime subscription
   registry has no app identity, so a write in app A is delivered to subscribers of app B
   that subscribed to a same-named table — a tenant-isolation break (live cross-app data
   leak) plus cross-app authorization on the VIA membership re-check. The file-boundary
   isolation guarantee is bypassed for the realtime channel.

3. **Eviction races / frees handles in use by other threads (HIGH).** The same missing
   "pin on borrow" lets a thread free an `app_db_t` whose `write_mtx`/connection another
   worker thread is about to lock or is using — destroy-while-locked UB under concurrent
   load.

4. **Unbounded GROUP BY result (HIGH).** Aggregate queries omit the `LIMIT` clause that
   every other read path enforces, so an authenticated caller can force the server to
   materialize a whole-table JSON array in one allocation burst (memory-exhaustion DoS).

---

## Critical

### C-1. Cross-registry use-after-free: `cel_apps` caches `app_db_t*` permanently while `app_db`'s LRU frees it at cap 64

- **Severity:** Critical
- **Dimension:** memory-safety / concurrency / resource management
- **Location:**
  - `src/engine/cel_apps.c:78` (`slot->db = db`, cached forever)
  - `src/core/app_db.c:139-142` (`evict_idle_locked` → `app_free`)
  - `src/core/app_db.c:114-122` (`app_free` closes handles, destroys mutexes, frees struct)
- **Merged from:** four independent reports (memory-safety, concurrency, resource-errorpaths)
  describing the identical defect.

**What's wrong.** Two stacked registries own the same `app_db_t*` with incompatible
lifetimes and bounds:

- `cel_apps` (`src/engine/cel_apps.c`) uses `CEL_APPS_MAX = 1024` (cel_apps.c:14) in a
  fixed array that never evicts at runtime. `open_into_cache()` stores the pointer from
  `app_db_get()` into `slot->db` (cel_apps.c:78) and keeps it for the process lifetime.
  `cel_apps_resolve()` short-circuits via `find_cached()` (cel_apps.c:58-62) and never
  re-resolves an already-seen host. `cel_apps_shutdown()` (cel_apps.c:126-131) never even
  clears `db`.
- `app_db` (`src/core/app_db.c`) uses `CEL_APP_MAX_OPEN = 64` (app_db.h:35). When the
  registry is full and a new app arrives, `app_db_get()` calls `evict_idle_locked()`
  (app_db.c:159), which swap-removes the LRU idle victim and `app_free()`s it
  (app_db.c:139-142): `sqlite3_close()` on each conn, `pthread_mutex_destroy()` on
  `pool_mtx`/`write_mtx`, `pthread_cond_destroy()`, and `free(db)`.

`evict_idle_locked` guards only on `checked_out == 0` (app_db.c:133); it has no knowledge
of the long-lived `cel_apps` reference. There is no refcount and no eviction callback. Once
a 65th distinct host is resolved, the `app_db_t` of an earlier host is freed while
`cel_apps` still holds the dangling pointer.

**Reachability.** `src/handlers/http_routes.c:548-551` reads the attacker-controlled Host
header and calls `cel_apps_resolve(host)`; line 562 calls `cel_apps_enter(app)`, which does
`app_db_set_current(app->db)` (cel_apps.c:117 → app_db.c:56), binding the freed pointer into
the thread-local. The next handler op — e.g. `app_db_conn_acquire(db)` →
`pthread_mutex_lock(&db->pool_mtx)` (app_db.c:176) or `app_db_write_lock` (app_db.c:219) —
runs on freed memory.

**Impact.** Remote, unauthenticated use-after-free in any multi-app deployment serving more
than 64 distinct Host bundles (the documented target topology). An attacker drives eviction
by issuing requests across >64 valid Host values, then re-requests an evicted one: locking a
destroyed mutex, walking a freed `conns[]` array, dereferencing closed `sqlite3*` handles.
Crashes, lock corruption, potential cross-app data exposure or control-flow corruption if
the freed slot is reallocated — a heap-grooming RCE primitive.

**Fix.** Make the two registries agree on lifetime. Minimum correct fix: `evict_idle_locked`
must not free an `app_db_t` that any `cel_app_t` still references. Concretely, one of:
- (a) Add a refcount/pin to `app_db_t`; `cel_apps` takes a ref on cache, `app_free` runs only
  at zero refs; or
- (b) Drop the cached `slot->db` and store `db_path` in `cel_app_t`, re-calling
  `app_db_get()` on every resolve (it returns the live/re-opened handle); or
- (c) Tie `CEL_APP_MAX_OPEN` to `CEL_APPS_MAX` and never evict an app referenced upstream.

---

### C-2. Realtime publish fans out across apps — cross-app data leak and cross-app authorization

- **Severity:** Critical
- **Dimension:** multi-app isolation
- **Location:** `src/engine/realtime.c:124` (`cel_realtime_publish`), match loop
  `src/engine/realtime.c:141-149`; subscription struct `src/engine/realtime.h:42-58`;
  VIA re-check `src/engine/api.c:991-1014`.

**What's wrong.** The realtime registry `g_list` (realtime.c:20) is a single process-global
linked list whose nodes (`rt_node_t`, realtime.c:14-18) carry only `{fd, cel_subscription_t}`.
`cel_subscription_t` (realtime.h:42-58) has **no app/host/db field** — a subscription is
identified purely by table name plus predicates. `cel_realtime_publish()` walks every node
in the process and matches solely with
`strcmp(n->sub.table, table) != 0 || !cel_rt_row_matches(...)` (realtime.c:142). There is no
app comparison anywhere in the loop.

Because the per-app auth schema (`cel_auth_schema_apply`) gives every app identically-named
tables (`cel_users`, etc.), a subscriber connected to app B that subscribed to a same-named
table receives app A's row whenever app A writes that table and the flat predicate matches.
EQ/OR subscriptions deliver the raw row with no per-publish DB check (realtime.c:146-148); a
superuser subscription has `npreds == 0`, which `cel_rt_row_matches` treats as match-everything
(realtime.c:69), so an app-B superuser streams **all** of app A's writes for any shared table.

**Reachability.** Every successful write reaches publish: `cel_handle_db_create/update/delete`
(data_handlers.c:31-33) binds app A via `cel_apps_enter(ctx->callback_data)` (data_handlers.c:18),
then `run_write` → `rt_emit` → `cel_realtime_publish` (api.c:208). The publish runs on the
writer's thread (app A bound) but iterates the global registry containing every app's subscribers.

**VIA authorization defect (compounding).** `g_member` is wired to `cel_api_rt_recheck_member`
(main.c:501) → `rt_membership` (api.c:991), which queries `app_db_current()` (api.c:993). Since
the callback runs synchronously on the publisher's (app A's) thread, an app-B subscriber's
membership is re-checked against app A's membership table — wrong database entirely.

**Impact.** Tenant-isolation break: a client of app B receives full row payloads (other tenants'
user records, messages, etc.) generated by writes in a different app A, with no shared trust.
Anyone who can register/control any app on the host can subscribe to common table names and
passively exfiltrate other apps' live write stream. The file-boundary isolation guarantee is
fully bypassed for the realtime channel.

**Fix.** Key the registry by app, not just table name:
- Add an app identity (the `cel_app_t*` or its stable host string) to `cel_subscription_t`,
  captured at subscribe time from `ctx->callback_data` in `cel_handle_subscribe`.
- Thread the publishing app into `cel_realtime_publish` (it is available via
  `app_db_current()` on the write path) and require `node.app == publishing_app` before
  delivering.
- Run `rt_membership` against the **subscriber's** app, not `app_db_current()`, so the VIA
  re-check validates membership in the correct database.

---

## High

### H-1. Eviction frees an `app_db_t` whose `write_mtx`/connection is in use by another thread (race) — destroy-while-locked UB

- **Severity:** High
- **Dimension:** concurrency
- **Location:** `src/core/app_db.c:126-144` (`evict_idle_locked`) and
  `src/core/app_db.c:114-122` (`app_free`), vs. the `app_db_current()` → acquire window in
  `src/engine/api.c:89-93`, `src/core/mfa.c:33-35`, and auth handlers.
- **Merged from:** two concurrency reports (the eviction race, and the "free a handle whose
  mutex may be held" report). The *shutdown* half of the latter was verified to be a **false
  positive** — `opcode_dispatcher_destroy` (main.c:560) joins all worker threads before
  `app_db_global_shutdown` (main.c:568), so teardown is correctly ordered — and is excluded.

**What's wrong.** `app_db_get()` returns a bare `app_db_t*` and drops `g_reg.mtx`
immediately (app_db.c:154/170) with no pin held. `evict_idle_locked` selects a victim solely
on `checked_out == 0` (app_db.c:132-134) and `app_free()`s it, unconditionally calling
`pthread_mutex_destroy(&db->write_mtx)` and `sqlite3_close()` on the conns (app_db.c:116-120).

A worker servicing a request runs `app=app_db_current()` (api.c:89), then
`app_db_write_lock(app)` (api.c:92), then `app_db_conn_acquire(app)` (api.c:93). In the window
**before** `app_db_conn_acquire` bumps `checked_out`, the app has `checked_out == 0` and is
fully evictable — including a writer that already holds `write_mtx` but has not yet acquired a
connection. If another thread fills the 64-slot registry and triggers eviction in that window,
it frees the victim while thread A is about to lock (or is inside) `pthread_mutex_lock` on the
destroyed `write_mtx`/`pool_mtx`. The same window exists for every `app_db_current()`→acquire
pair in `mfa.c` and `auth.c`.

Note the victim is a *prime* LRU candidate: `last_used` is bumped only inside `app_db_get`
(app_db.c:152), and the cached-resolve path (cel_apps.c:97-98, `find_cached`) returns without
calling `app_db_get`, so a hot, long-open app keeps a stale-low `last_used` and is
preferentially evicted.

**Reachability.** The server runs concurrent worker pools — DB pool 4, CPU pool 2
(main.c:488-491), WS listener threads 4 (main.c:506). One thread resolving a 65th host can
free the app another worker is mid-write on.

**Impact.** Cross-thread use-after-free / lock on a destroyed mutex under concurrent load
whenever the open-app set churns past 64 — non-deterministic crashes and memory corruption
reachable by remote traffic across many hosts. Distinct from C-1 in that it does not even
require 64 *engine-cached* apps, only 64 concurrently-open in the core registry.

**Fix.** Pin the handle for the duration of a checkout: atomically increment a refcount (or
`checked_out`) while still holding `g_reg.mtx` inside `app_db_get()`, and let eviction free
only handles with zero refs **and** zero borrows. Release the pin when the request unbinds.
Eviction must never free a handle reachable via a thread-local binding or an in-flight op.
(This shares the root cause with C-1; a single refcount/pin fixes both.)

### H-2. Aggregate (GROUP BY) query has no `LIMIT` → unbounded result materialized in memory (DoS)

- **Severity:** High
- **Dimension:** resource / error-paths
- **Location:** `src/engine/query_builder.c:520-583` (`build_aggregate_body`, no LIMIT),
  wrapped by `cel_build_aggregate` (query_builder.c:585-596); reached via
  `src/engine/api.c:487-490` (`run_rows`) and `src/engine/result_json.c:84-95`
  (`cel_stmt_result_to_json`).

**What's wrong.** `cel_build_list`/`cel_build_get` append a `LIMIT` clamped to
`CEL_LIST_MAX_LIMIT = 1000` via `build_limit_offset` (query_builder.c:347-364), but
`build_aggregate_body` emits `SELECT ... GROUP BY <cols> ORDER BY <cols>` and returns at
line 582 with **no LIMIT** and never calls `build_limit_offset`. Group columns are validated
only as real columns (query_builder.c:534), not as low-cardinality — a PK/UUID/text column
yields one output row per table row.

**Reachability.** `GET /api/<table>` routes through `build_list_req` → `cel_api_list`
(http_routes.c:461-462), with `?group=`/`?aggregate=` mapped straight through via
`add_csv_array` (http_routes.c:139-140). `cel_api_list` (api.c:469-499) requires only
authentication + LIST policy; when the group/aggregate arrays are non-empty it short-circuits
to `cel_build_aggregate` + `run_rows(&aq, NULL, ...)` (api.c:487-490), **before** the keyset
path whose `CEL_LIST_MAX_LIMIT` clamp (api.c:569) would apply. `run_rows` with `t == NULL`
calls `cel_stmt_result_to_json`, which steps the entire result set
(`while (sqlite3_step == SQLITE_ROW)`) building one cJSON object per row with no row cap. The
WS path (`cel_handle_db_list/query`, data_handlers.c:29,36) reaches the same code.

**Impact.** An authenticated caller with LIST permission on any table can request
`?group=<high-cardinality-col>` and force the server to materialize a JSON array sized to the
whole table in a single allocation burst on one of only 4 DB worker threads — memory-exhaustion
DoS plus worker tie-up for the duration.

**Fix.** Append a bounded `LIMIT <CEL_LIST_MAX_LIMIT>` in `build_aggregate_body` (honoring an
optional caller `limit` capped to the max), exactly as `cel_build_list` does via
`build_limit_offset`, so aggregate cardinality is bounded like every other read path.

---

## Medium

### M-1. No per-account cap on MFA (TOTP) verification attempts — per-challenge cap bypassed by re-issuing challenges

- **Severity:** Medium
- **Dimension:** auth / crypto
- **Location:** `src/core/mfa.c:248-308` (`cel_mfa_verify_login`),
  `src/core/mfa.c:228-245` (`cel_mfa_create_challenge`), `src/core/auth.c:182-186` and
  `src/core/auth.c:302-304` (challenge minted on every factor-1 success).

**What's wrong.** `cel_mfa_verify_login` enforces `MAX_CODE_ATTEMPTS = 5` **per challenge
row** only: it selects `attempts` for the specific token (mfa.c:268-273), burns the challenge
at `attempts >= 5` (mfa.c:275-279), and a failed guess only does
`UPDATE cel_mfa_challenges SET attempts = attempts+1 WHERE token = ?1` (mfa.c:287). There is
no per-user accumulation. A fresh challenge is minted on every factor-1 success
(`cel_auth_login` → `cel_mfa_create_challenge`, auth.c:182-185, and the OAuth path
auth.c:302-304), with no per-user cap consulted.

The factor-1 lockout machinery (`cel_users.failed_login_count`/`locked_until`) provides no
brake here: it increments only on a wrong password / inactive account (auth.c:154-161) and
**resets** on password success (auth.c:162-167). An attacker who already knows the password
passes factor-1 every time, so this counter never trips and in fact resets.

The only global brake on `/auth/mfa/verify` is the per-IP token bucket `g_auth_rl`
(http_routes.c:316-324), bypassable via IP rotation / shared CGNAT. With `TOTP_WINDOW = 1`
(mfa.c:18) there are ~3 valid 6-digit codes out of 1e6 at any instant; unlimited challenge
re-issuance (5 guesses each) plus IP rotation makes the second factor brute-forceable.

**Reachability.** `/auth/mfa/verify` is a public POST route (http_routes.c:317) →
`cel_api_mfa_verify` (api.c:780-788) → `cel_mfa_verify_login`.

**Impact.** Account takeover of an MFA-protected account once the password is known
(phished/reused). The TOTP secret never rotates during the attack. Medium because it requires
the victim's password to already be compromised.

**Fix.** Add a per-user MFA failure counter with lockout (reuse
`failed_login_count`/`locked_until`, or add a `cel_mfa` failed-attempt column) so that N
failed TOTP/recovery attempts across **any** challenge within a window locks MFA verification
for that user. Do not rely on the per-challenge cap or per-IP limit alone.

---

## Low

### L-1. Login enumeration timing oracle if the decoy-hash precompute fails at startup

- **Severity:** Low
- **Dimension:** auth / crypto
- **Location:** `src/core/auth.c:23-27` (`cel_auth_init`), `src/core/auth.c:137`
  (not-found path), `src/core/auth.c:148` (found-but-null-secret path); startup call
  `src/main.c:398`.

**What's wrong.** The constant-time-equalizing decoy verification at auth.c:137 and 148 is
guarded by `if (g_decoy_hash[0])`. `cel_auth_init` (auth.c:23-27) returns `void` and, if
`cel_password_hash` fails, sets `g_decoy_hash[0] = '\0'`. `cel_password_hash`
(password.c:19-28) calls `crypto_pwhash_str` with INTERACTIVE limits, whose realistic failure
mode is allocation failure of the ~64 MiB Argon2id buffer — rare but genuine. `main.c:398`
calls `cel_auth_init()` with no return check, so a failed decoy precompute is not fatal.

In that state the not-found branch does no Argon2id work and returns `CEL_AUTH_INVALID`
immediately, while a found user with a secret always pays the full `cel_password_verify`
(auth.c:146) — re-introducing the registered-vs-unregistered login-latency oracle the decoy
exists to close.

**Impact.** Account-existence enumeration via login latency in the rare event the decoy failed
to initialize.

**Fix.** Make decoy-init failure fatal (refuse to start), or on an empty decoy still perform an
equivalent dummy Argon2id verify on every not-found/null-secret path so timing stays uniform.

### L-2. Non-constant-time comparison of the `/metrics` bearer token

- **Severity:** Low
- **Dimension:** auth / crypto
- **Location:** `src/handlers/http_routes.c:205` (in the `/metrics` branch, lines 197-213).

**What's wrong.** After a length check (`blen != tlen`), the presented Bearer token is compared
to `CEL_METRICS_TOKEN` with `memcmp(bearer, mtok, tlen)` — a short-circuiting byte-by-byte
compare that returns on the first mismatching byte, leaking per-byte match progress via timing.
This is inconsistent with the codebase's own constant-time practice (e.g. `sodium_memcmp` in
`src/core/totp.c:106`); libsodium is already a hard dependency.

**Reachability.** The `/metrics` branch is reached unconditionally from the router
(http_routes.c:563) for `GET /metrics`.

**Impact.** In principle a network attacker can recover the metrics token byte-by-byte via
timing and then scrape `/metrics` (which leaks build version and live auth/operational
telemetry). Low because metrics scraping is the only thing gated, the endpoint 404s when the
token is unset, and network timing channels are noisy.

**Fix.** Compare with a constant-time primitive (`sodium_memcmp` / `CRYPTO_memcmp`) after the
length check.

---

## Notes on merges and exclusions

- **C-1** consolidates four separately-filed reports (titled across the memory-safety,
  concurrency, and resource-errorpaths dimensions) describing the identical cross-registry
  `app_db_t` use-after-free. They share one root cause and one fix.
- **H-1** consolidates two concurrency reports of the same "eviction without a pin" defect
  (the in-flight-checkout race and the destroy-while-locked-mutex report). The **shutdown**
  half of the latter was verified to be a false positive: `ws_server_destroy` (main.c:558)
  and `opcode_dispatcher_destroy` (main.c:560, which `pthread_join`s all workers) run before
  `app_db_global_shutdown` (main.c:568), so shutdown teardown is correctly ordered and is not
  reported.
- A single refcount/pin on `app_db_t` (taken under `g_reg.mtx` in `app_db_get`, released at
  request unbind, with `app_free` gated on zero refs) is the common remedy for both C-1 and
  H-1.
