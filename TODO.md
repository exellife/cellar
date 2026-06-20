# cellar — TODO

cellar is a **multi-app backend engine**: same split as the family — **portico is the
transport (mechanism)**, **cellar is the engine (policy)**. Where pgforge is *one app /
many tenants* over a shared Postgres (isolation by a `tenant_id` column), cellar is
*many apps / each its own database* — **one SQLite file per app** (isolation by the
file). That makes per-app schemas trivial: different apps can have entirely different
tables/columns (the thing pgforge's single global catalog can't do).

Forked from pgforge (clean copy, no history). Right now it is still byte-for-byte
pgforge and builds *as* pgforge — the rename and the engine pivot are the work below.

---

## Phase 0 — establish the project

- [x] **Baseline commit.** Verbatim pgforge copy committed (`82045ef` "fork from pgforge:
      baseline copy") so the rebrand + re-engine diffs stay clean and reviewable.
- [x] **Rebrand `pgforge → cellar`** (146 files). Source identifiers (`pgf_*` → `cel_*`),
      env vars (`PGF_*` → `CEL_*`), CMake project + binary target (`project(cellar)`,
      `add_executable(cellar)`), README, default DB name, `dist/pgforge.{service,env} →
      cellar.{service,env}`. **Scope rule:** narrative/historical docs (`PLAN`, `VISION`,
      `MULTI_APP`, `SECURITY_AUDIT`, `docs/*`, `TODO`) were left untouched — their
      `pgforge`/`pgf_` refs name the *upstream* project & its commit history. Verified:
      builds, runs as `cellar 0.1.0`, all pure unit tests pass (only Postgres-dependent
      e2e tests fail, expected — no `cellar` DB provisioned). Still Postgres-backed; pure
      rename. Also fixed a *pre-existing* `cjson` duplicate-target collision (cellar +
      portico both registered one) — cellar now reuses portico's target.
- [x] **Design doc** — [`docs/cellar-design.md`](docs/cellar-design.md). Locks the
      architecture: per-app SQLite (isolation by file), the blocking-vs-event-loop
      concurrency contract (handle cache + per-app write serialization on worker
      threads), per-app catalog, the **3-layer extension model** (SQLite-native → **Lua
      hooks** over a fixed contract → compiled-C escape hatch; WASM later for untrusted
      apps), the **app-as-bundle** format (`data.db` + `hooks.lua` + `policies.json`),
      identity/control-plane, and ports-vs-rewrites. Settled decisions: **first-party
      apps for now** (→ Lua, no heavy sandbox) and **home-behind-tunnel as primary
      deployment** (data lives at home; portico-tunnel relay only moves ciphertext).

## Phase 1 — the engine pivot (Postgres → SQLite)  [scoped by the design doc]

Reuse: portico (transport), `opcode_dispatcher`, `logger`, the generic-opcode CRUD
pattern, and the policy-engine concept. Rewrite the DB layer:

- [~] **`db_connection.c` → per-app SQLite layer.** Open-by-file, an LRU cache of
      `sqlite3*` handles, WAL mode, and a **per-app write queue** so one write-heavy app
      can't starve others (SQLite is single-writer per file; never touch the event
      thread — every call runs on the worker pool).
      **Foundation done:** new module `src/core/app_db.{c,h}` — open-by-file (lazy, WAL +
      `synchronous=NORMAL` + `foreign_keys=ON` + busy_timeout), per-app pooled handles
      (`NOMUTEX`, concurrent readers), per-app `write_mtx` serialization, bounded LRU
      registry of open apps (FD ceiling), file isolation. Unit-tested (`tests/app_db_test.c`,
      ctest `app_db`: 14 checks incl. concurrent writers/readers). **Still to do:** retire
      the libpq `db_connection.c` once the consumers (catalog, query_builder, api, auth,
      mfa, migrate, row_json) move onto `app_db`; wire it into the `opcode_dispatcher`
      worker pool; pair with the per-app `lua_State` (Phase 2).
      _SQLite sourcing:_ built against system libsqlite3 (3.45.1; dev files staged from the
      Ubuntu `.deb` into gitignored `build-deps/sqlite`, since sqlite.org is unreachable
      here and there's no sudo). CMake discovers it via `-DSQLITE3_ROOT` / `libsqlite3-dev`.
- [~] **Result serializer → SQLite (`row_json.c` → `result_json.c`).** New libpq-free
      module `src/engine/result_json.{c,h}`: serialize a stepped `sqlite3_stmt` → typed
      JSON, preserving the existing API contract (catalog-typed cells: int/real → number,
      declared bool → true/false, json column parsed, NULL → null; runtime-typed table-less
      path for RPC; BLOB → hex). Unit-tested (`tests/result_json_test.c`, ctest
      `result_json`: 17 checks, in-memory SQLite, no Postgres). **Still to do:** delete the
      libpq `row_json.c` once `api.c` switches to it (Step 5).
- [~] **`schema_catalog.c` → PRAGMA introspection.** `sqlite_master` +
      `PRAGMA table_info / foreign_key_list / index_list` instead of `information_schema`;
      the catalog becomes **per-app** (replaces today's single global `g_active`).
      **Introspector done:** new libpq-free module `src/engine/schema_catalog_sqlite.c` —
      `cel_catalog_build_sqlite(sqlite3*)` builds the same `cel_catalog_t` from
      `sqlite_master` + `pragma_table_info` / `pragma_foreign_key_list` (table-valued,
      name bound — injection-safe); declared-type → engine-type **affinity** mapping
      (INTEGER→bigint, BOOLEAN→bool, JSON→json, REAL→float, TIMESTAMP→timestamptz, …);
      PK/FK/nullable/default flags; excludes `cel_*` + `sqlite_*`. Unit-tested
      (`tests/schema_catalog_sqlite_test.c`, ctest `schema_catalog_sqlite`: 24 checks).
      **Still to do:** drop the libpq `cel_catalog_build` + global `g_active` once routing
      gives each request its app's `sqlite3*` (Step 5); wire per-app catalog caching +
      invalidation on schema change.
- [x] **`query_builder.c` → SQLite dialect.** `?`/`:name` placeholders (not `$N`),
      `RETURNING` (SQLite ≥ 3.35), type-affinity handling for JSON serialization.
      **Done (in place — the builder is pure/libpq-free):** `$N` → `?N` (numbered, single
      `q_placeholder` chokepoint, no reuse); booleans bind as `1`/`0` (SQLite has no bool);
      `ilike` → `LIKE` (SQLite LIKE is already case-insensitive); `RETURNING` kept;
      `cel_build_rpc` left PG-shaped with a deferral note (SQLite RPC → the hook layer,
      Phase 2). Regression test repinned to `?N` (`tests/query_builder_test.c`). Added an
      **end-to-end test** `tests/query_exec_sqlite_test.c` (ctest `query_exec_sqlite`):
      build → bind → step → serialize on a real in-memory SQLite db across
      create/list/update/LIKE/delete with bool round-trip + owner scoping — its `run()`
      helper prototypes the Step-5 executor. **Note:** in-place conversion means the
      still-live PG `api.c` now builds `?N` SQL it can't run against Postgres — moot (no
      live PG; deleted in Step 5), build stays green.
- [~] **Delete the tenant machinery.** No `tenant_id` column, no `set_config` /
      `SET LOCAL`, no RLS — isolation is the file boundary. Removes `run_rows`' tenant
      context entirely.
      **Done (data path):** `api.c` `run_rows` rewritten as a SQLite executor (prepare →
      bind `?N` text/NULL → step → `cel_stmt_*_to_json`; per-app `write_lock` on writes;
      SQLite error→HTTP mapping with extended result codes — UNIQUE→409, FK/NOTNULL/CHECK→
      400). Removed `rls_tenant_setting`, the `run_rows_pipelined` PG fast-path, the tenant
      rule in `make_scope`, and every `cel_tenancy_column()` gate (rpc/oauth/register/
      create_user); `rt_membership` (realtime VIA) now queries the app's SQLite db.
      `cel_tenancy_init`/`cel_tenancy_column` deleted from `policy.c/.h`. RPC deferred (501;
      → hook layer, Phase 2); the SECURITY-DEFINER audit is a no-op. `main.c` opens one app
      from `CEL_DATA_DB` + introspects its catalog (active) + `cel_api_set_app`; auth still
      on the Postgres pool (transitional hybrid). api.c is now libpq-free. Verified: clean
      build, all 13 C unit tests pass, binary boots and introspects a real SQLite schema
      (internal `cel_*` excluded). **Still to do:** the vestigial `tenant_id` struct fields
      + the PG-migration tenant tooling (`sql/tenancy/`, migrate.c CLIs, main.c tenant
      commands) come out with the auth/migrate conversion; per-app catalog (drop global
      `g_active`) comes with routing.
- [~] **Identity layer → per-app SQLite (5b).** `auth.c` fully converted (libpq-free): all
      flows — login (decoy-hash timing, lockout), register, verify, logout, oauth-link,
      password reset, email verification, create_user, seed — run against the app's SQLite db
      via `app_db_current()`. New: `app_db_set_current/current` seam (core); `cel_uuid_v4`
      (libuuid — SQLite has no `gen_random_uuid()`); `auth_schema.c` (consolidated SQLite
      identity DDL: TEXT uuids, INTEGER epoch times, 0/1 bools) applied per-app at boot. main.c
      seeds into the app after it's current; the PG pool now only matters for not-yet-converted
      mfa.c / migrate.c. Unit-tested (`tests/auth_sqlite_test.c`, ctest `auth_sqlite`: 26
      checks); boot+seed verified with **no Postgres**. **mfa.c also converted** — TOTP
      enroll/confirm/disable/regenerate, login challenge + verify (TOTP or single-use recovery
      code), required-for, and admin reset all on the app's SQLite db; the MFA bypass seam is
      closed. Extracted the SQLite bind/exec helpers into `src/core/db_sqlite.{c,h}`
      (`cel_db_prep/exec/one_text`, `cel_now_epoch`), shared by auth + mfa (+ migrate next);
      auth.c refactored onto them. Tested: `tests/mfa_sqlite_test.c` (ctest `mfa_sqlite`,
      end-to-end with real TOTP codes). 15/15 C unit tests green.
- [x] **Drop migrate.c + Postgres entirely (5b-final).** Decided (file-per-app → no global
      schema to version): **deleted** `migrate.{c,h}`, `db_connection.{c,h}`, `row_json.{c,h}`,
      the PG `cel_catalog_build` path, `sql/migrations|demo|tenancy/`, `cmake/embed_migrations.cmake`,
      and the migrate/tenant CLIs (`migrate`, `tenancy-protect`, `create-{tenant,platform-admin}`,
      `{suspend,resume,export}-tenant`). main.c lost `init_db`/the PG pool; `revoke-sessions` /
      `mfa-reset` / `unlock` now open the SQLite app (`cli_open_app`). Schema setup is per app:
      `auth_schema.c` is **version-stamped via `PRAGMA user_version`** (v1 today; future cellar
      versions append ALTER steps to evolve existing bundles); the user's app schema is owned by
      its `data.db`. CMake dropped `find_package(PostgreSQL)` + the link → **the binary no longer
      links libpq** (`ldd | grep pq` = 0). dist/ templates updated (CEL_DATA_DB, StateDirectory,
      no `migrate` pre-start). Net −1646 lines; 15/15 unit tests green; boots/seeds/runs admin
      CLIs on SQLite with no Postgres. **The Postgres→SQLite engine pivot is functionally
      complete — no hybrid remains.** (Leftover cleanup: vestigial `tenant_id` struct fields in
      auth.h/policy.h; README still describes the Postgres era.)
- [~] **App routing + provisioning.** Resolve app from the request → its bundle.
      **Done (HTTP):** decided one cellar process hosts many apps via **in-process Host→bundle
      routing** (not process-per-app; the routing key is the full Host, not subdomain parsing —
      see [[cellar-vision-in-process-multiapp]]). New `src/engine/cel_apps.{c,h}` registry:
      `CEL_APPS_DIR` → multi-app (`<dir>/<host>/data.db`, opened lazily, schema applied, catalog
      cached, host validated against path-traversal); else single-app from `CEL_DATA_DB` (the
      process default). Request context is now **thread-local with a process-default fallback**:
      `app_db_current()` / `cel_catalog_active()` return the per-thread binding (set per HTTP
      request by `cel_apps_enter` in `cel_http_router` from the Host) else the boot default — so
      auth/mfa/api code is unchanged and concurrent requests hit different apps. **WS routes too**
      (Option A): portico captures the handshake Host (`ws_connection.host`, commit 715111f),
      delivers it via the per-connection `user_data`; cellar resolves the app in `on_binary_message`,
      carries it on the opcode ctx (`callback_data`), and binds it in each WS handler
      (data/auth/schema/subscribe). Unknown Host → 404 (HTTP) / op error (WS). Tested:
      `tests/cel_apps_test.c` (ctest `cel_apps`, 20 checks) + **live HTTP + live WS proofs** —
      register/login on a.com works, the same on b.com fails (isolated per-app users) over both
      transports; single-app back-compat verified. **Still to do:** **realtime fan-out is not yet
      app-scoped** — the `cel_realtime` registry is keyed by table name globally, so a write in
      app A could match an app B subscriber of a same-named table (scope the registry/subscription
      by app); provisioning ergonomics (drop-a-dir works; an admin API later); per-app catalog
      invalidation on schema change; the **control-plane DB** (platform admin / global registry /
      app lifecycle).

## Phase 2 — per-app extensibility (the hook layer)  ✅ COMPLETE  [design §7-9]

- [x] **Lua hook layer (LuaJIT, vendored from source).** The full contract is live and
      atomic: `authorize / before / after / rpc / on_realtime`, dispatched per request
      against the current app. Per-(worker-thread × app) `lua_State` cache, lazy from
      `hooks.lua`, mtime hot-reload, pcall-isolated (fail-closed). FFI surface (opaque
      `cel_val` handles + `cellar.query/exec/log`) — no hand-written bindings. `before`+
      write+`after` run in one request transaction (hook writes roll back with the main
      write); `after` is post-commit.
- [x] **Bundle tooling.** `cellar provision` (scaffold data.db + hooks.lua + public/ +
      seeded admin); per-bundle static serving of `public/` (SPA fallback); `export`/
      `import` (consistent live `VACUUM INTO` snapshot, rehost); per-app `policies.json`;
      control-plane registry (`CEL_CONTROL_DB`) with `suspend`/`resume`/`apps` lifecycle.
- [ ] **Layer 1 (free, declarative):** lean harder on SQLite-native logic — triggers,
      `CHECK`, `STRICT`, generated columns, views — the engine already respects them; this
      is guidance/examples, not engine work.
- [ ] **Compiled-C escape hatch** for first-party hot-path handlers (registered by name).
- [ ] *(later)* **WASM runtime** — same contract, sandboxed — once apps can be third-party.

## Backlog — deferred features (post-Phase-2, before/alongside Phase 3)

- [ ] **Platform-admins over an API (control-plane management API).** Today the
      control-plane (design §11) is CLI-only: `provision / export / import / suspend /
      resume / apps`, run with shell access to the host. The `platform_admin` role exists
      but is created out-of-band and blocked from in-band creation, and there is **no API**
      for a global admin to manage the fleet (list / provision / suspend apps, view status)
      remotely or from a dashboard.
      _Shape:_ platform admins live in the **control DB** (`cel_control`), not in any app's
      `data.db`; a set of authenticated endpoints (e.g. under a reserved host or `/_control/…`
      path) gated to `platform_admin`, wrapping the existing `cel_control_*` ops + provision/
      export. Decide the routing surface (reserved Host vs path) and how platform-admin auth
      is bootstrapped. _Touches:_ `cel_control.{c,h}`, a new control handler, routing in
      `cel_http_router`.

- [ ] **In-DB `_hooks` (single-file bundle).** Option to store the hook source in a
      `_hooks` table inside `data.db` instead of (or in addition to) the on-disk
      `hooks.lua`, so an app's schema + data + **behavior** travel as ONE file (design §14).
      _Shape:_ `cel_hook_state` loads from the `_hooks` table when present (file as the
      fallback / dev path); a version/generation column replaces the mtime check for
      hot-reload; decide precedence (file vs table) and the table schema, e.g.
      `_hooks(name TEXT PK DEFAULT 'main', source TEXT, updated_at INTEGER)`. Makes
      `export`/`import` reduce to copying the `.db`. _Touches:_ `cel_hook_state.{c,h}`,
      provision, export/import.

- [~] **Offline-first device sync.** Let clients (esp. Flutter) hold a local SQLite mirror
      and sync across devices through cellar. Designed in
      [`docs/cellar-sync-design.md`](docs/cellar-sync-design.md): v1 is server-authoritative
      **delta sync** — per-app monotonic `rev` cursor + `deleted` tombstones on syncable
      tables, `sync_pull(since)` / `sync_push(mutations)` RPCs through the existing
      `authorize`/`before` hooks, last-write-wins with a `resolve(...)` hook override, and the
      realtime feed as the online fast-path. CRDT (cr-sqlite) is the later opt-in upgrade.
      **Slice 0 DONE (the rev + tombstone substrate; commits T1–T7):** detect-by-columns opt-in
      (`rev`+`deleted`), per-app `_sync_seq` + `next_rev()`, engine stamps rev / force-owns
      rev+deleted, DELETE→soft-delete, reads hide tombstones; e2e `sync_rev` + dogfooded on
      `tasks_app`. **Slices 1 & 2 DONE:** `POST /sync/pull` (delta read; tombstones; safe multi-table cursor) and
      `POST /sync/push` (batch all-or-nothing; client-id creates; LWW + `resolve()` hook override;
      e2e `sync_pull`/`sync_push`). **Next slices:** (3) per-device cursors + tombstone GC;
      (4) client reference impl (offline loop in `examples/`); (+) `resolve()` field-level merged-row
      return. **Known gap to close:**
      hook-issued `cellar.exec` writes bypass the substrate (no rev stamp, no auto soft-delete /
      tombstone-filter) — expose a sync-aware hook helper (`cellar.delete`) or stamp inside
      `cellar.exec` for syncable tables (see design note §6, last bullet).

- [ ] **MFA: per-app mode + expose `required`.** TOTP 2FA is fully live (enroll/confirm/
      disable/recovery-codes/verify routes, per-user lockout, `mfa-reset` CLI, `mfa_sqlite`
      test) but two gaps: (1) **mode is process-wide.** `CEL_MFA` is read once at boot →
      `cel_mfa_set_mode` (`src/main.c` ~773), so the on/off toggle is per-process while the
      enrollment *data* is per-app (`cel_mfa` in each `data.db`). To make MFA a per-app
      decision like `policies.json`, move the mode into the per-app policy/config and resolve
      it per request (thread-local, mirroring `cel_policy` `active()`). (2) **`required` isn't
      wired.** `CEL_MFA_MODE_REQUIRED` exists in the enum and `mfa.c` has a `required_for`
      notion, but the env parser only maps `"optional"` (else `off`) — there's no way to force
      enrollment fleet- or app-wide. Decide the surface (env `CEL_MFA=required` for the global
      case; a `policies.json` key for the per-app case) and the UX for a user who must enroll
      before they can do anything. _Touches:_ `src/core/mfa.{c,h}`, `src/main.c`, `policy.{c,h}`,
      and the login/`/auth/mfa/*` flow in `api.c`.

## Performance — findings & future work  [from the srvlab bench/profile pass]

Measured on **srvlab** (32-core x86_64, NVMe SN850X, plain HTTP over LAN), `bench.sh`
+ `multiapp_write.sh`, `perf` on the write path. WAL + `synchronous=NORMAL` (fsync at
checkpoint, not per commit).

- [x] **Prepared-statement cache** (commit `bdaf084`). Re-parsing+re-planning identical
      SQL every request was a top write-path CPU cost (profile: `sqlite3_prepare_v2`/
      `sqlite3RunParser` ≈ `sqlite3_step`, ~6.7%+17.6% inside `cel_api_create`). Added a
      per-connection cache (`app_db_stmt_cached`, FIFO, 64 stmts, `SQLITE_PREPARE_PERSISTENT`);
      `run_rows_on` now `reset`s instead of `prepare`/`finalize`. **Result: single-app write
      4.6k→5.9k req/s (+29%), p50 6.05→4.43 ms; authed reads +3–4%; cached read +8%.**
      _Note:_ `prepare_v3` auto-reprepares on `SQLITE_SCHEMA`, so a cached stmt survives a
      DDL change safely — no manual cache invalidation needed on `ALTER`/`CREATE`.

The remaining ceilings, in rough priority for a future perf pass:

- [ ] **Single-app write is lock-bound, not CPU-bound.** One `write_mtx` per app file
      serializes writers; with the cache, prepare is gone but `BEGIN IMMEDIATE`/`COMMIT`
      round-trips per request dominate. **Group commit** (coalesce N pending writes from
      the per-app queue into one transaction + one checkpoint-eligible commit) is the next
      big lever — amortizes the txn/WAL overhead across concurrent writers to the same app.
- [ ] **Multi-app write aggregate plateaus (~39k/s at 8 apps) because the co-located Python
      generator saturates the same 32 cores** — that's a *measurement* artifact, not a
      server ceiling. To find the true ceiling: drive from a second machine, or replace
      `loadtest.py` with a native tool (`wrk`/`hey`). The harness already documents the
      cross-machine recipe; wire it into a repeatable script.
- [ ] **Reader concurrency caps at `CEL_APP_CONNS_PER_APP` (4).** Read-heavy apps could use
      more handles; make it tunable per app (bundle config) or adaptive, balanced against
      the open-FD ceiling (`CEL_APP_MAX_OPEN`).
- [ ] **Extend the statement cache to the still-uncached per-request SQL.** The AFTER
      profile (write path) shows execution now dominates parse (`sqlite3VdbeExec` 2.2% >
      `sqlite3RunParser` 1.2%), but parser symbols (`yy_reduce`, `sqlite3GetToken`,
      `sqlite3PExpr`) are still ~3–4% combined — because two hot paths bypass the cache:
        1. **Auth/token resolution** (`cel_sessions ⋈ cel_users`, every authed request) goes
           through `cel_db_prep` in `src/core/db_sqlite.c` → `sqlite3_prepare_v2`+finalize
           each call. Route it through `app_db_stmt_cached` (or give `db_sqlite` its own
           cache). Biggest single remaining parse win — it's on *every* request, read & write.
        2. **Transaction control** — `BEGIN IMMEDIATE` / `COMMIT` / `ROLLBACK` use
           `sqlite3_exec` (api.c ~343–362), which re-parses each tiny statement per write.
           Hold three persistent prepared stmts per connection and `step`+`reset` them.
        3. `cel_identity_from_token`'s direct `sqlite3_prepare_v2` (api.c ~1175).
- [ ] **Read path (~40–60k/s) — profile it next** (only the write path was profiled). Likely
      split across the token resolution above, cJSON serialization, and per-request routing
      (`cel_apps_enter`/`leave`). Candidate wins: a faster/streaming JSON writer; caching the
      built `cel_query_t` per (route, shape).
- [ ] **Login (Argon2id) ≈ 33/s on srvlab — CPU-bound by design.** Already rate-limited +
      lockout-guarded. Make the Argon2 cost params deployment-tunable so operators trade
      hardening vs. throughput per box.
- [ ] **Bench reproducibility.** `bench.sh`/`multiapp_write.sh` are manual (numbers, not
      pass/fail). Consider a checked-in baseline + a `make bench` that records to a file so
      regressions are visible across commits.

## Phase 3 — deploy as a home origin behind portico-tunnel

- [ ] Run cellar on the home box (`srvlab`) as the origin; portico-tunnel SNI-routes each
      app's domain through the relay (passthrough, never decrypts), cert via
      ACME-through-tunnel. Data (the `.db` bundles) stays at home → sovereignty.
- [ ] **Backups** — the durability story is now the host's: Litestream or scheduled
      rsync of the `apps/` bundle dirs.

## Open decisions (the rest; see design doc §14)

- [~] **Hooks: file vs in-db** — shipped the on-disk `hooks.lua` path; the in-db `_hooks`
      table is captured in the Backlog above (single-file bundles).
- [x] **`after` transactionality** — decided: `before` runs in the request txn (its writes
      roll back with a failed/denied write), `after` runs post-commit.
- [ ] **Per-app routing** — subdomain-per-app (SNI forward each) vs path prefix.
- [x] **Vendor SQLite:** the amalgamation (`sqlite3.c`, public domain) is committed
      under `third_party/sqlite` (3.45.1) and compiled into the build — no system
      dependency, reproducible from a clean clone (mirrors `third_party/cjson`). Still
      open: decide `STRICT` tables + CHECK constraints for type fidelity.
- [ ] **Naming convention** for the rebranded symbols (`cel_*` / `CEL_*`?).

---

## Notes / context

- pgforge's old backlog and the multi-app rationale live in the source project; the key
  conceptual writeups to carry forward are `MULTI_APP.md` and `VISION.md` (the
  mechanism/policy decision rule).
- portico-side companion work (domain → app routing via a config file + SIGHUP reload)
  is sketched in `portico/docs/portico-config-design.md` — cellar is the engine that
  config would point each domain at.
