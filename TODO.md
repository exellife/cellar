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
      end-to-end with real TOTP codes). 15/15 C unit tests green. **Next:** migrate.c → SQLite,
      then delete db_connection.c / row_json.c / PG schema_catalog path / vestigial `tenant_id`
      fields / PG tenant tooling.
- [ ] **App routing + provisioning.** Resolve app from the request (Host/path) → its
      bundle. A small **control-plane DB** (app registry, domain→bundle routing,
      platform admin). Decided: **per-app users** in each `data.db` + a control-plane for
      routing/admin. Provision = drop a bundle dir + register; delete = `rm -rf`;
      export = copy.

## Phase 2 — per-app extensibility (the hook layer)  [scoped by the design doc §7-9]

- [ ] **Layer 1 first:** make CRUD respect SQLite-native logic — triggers, `CHECK`,
      `STRICT`, generated columns, views, SQL-function RPC (a lot of business logic lives
      here, in the `.db`, for free).
- [ ] **Lua hook layer (LuaJIT).** Link **LuaJIT** (GC64); implement the fixed hook
      contract (`authorize / before / after / rpc / on_realtime`); a per-app `lua_State`
      cache paired 1:1 with the per-app `sqlite3*` handle on each worker thread (one state
      per thread×app, no locking); expose a generous first-party API **via FFI** (app DB,
      request ctx, logging, guarded outbound — no hand-written bindings); `hooks.lua`
      edit → drop cached state = hot-reload.
- [ ] **Compiled-C escape hatch** for first-party hot-path handlers (registered by name).
- [ ] *(later)* **WASM runtime** — same contract, sandboxed — once apps can be third-party.

## Phase 3 — deploy as a home origin behind portico-tunnel

- [ ] Run cellar on the home box (`srvlab`) as the origin; portico-tunnel SNI-routes each
      app's domain through the relay (passthrough, never decrypts), cert via
      ACME-through-tunnel. Data (the `.db` bundles) stays at home → sovereignty.
- [ ] **Backups** — the durability story is now the host's: Litestream or scheduled
      rsync of the `apps/` bundle dirs.

## Open decisions (the rest; see design doc §14)

- [ ] **Hooks: file vs in-db** — `hooks.lua` on disk vs a `_hooks` table in `data.db`.
- [ ] **`after` transactionality** — in-txn (atomic) vs post-commit (safe side-effects).
- [ ] **Per-app routing** — subdomain-per-app (SNI forward each) vs path prefix.
- [ ] **Vendor SQLite:** add the amalgamation (`sqlite3.c`, public domain) under
      `third_party/`; decide `STRICT` tables + CHECK constraints for type fidelity.
      _Status:_ deferred — sqlite.org is unreachable in this env, so v1 links **system
      libsqlite3** (`libsqlite3-dev`, or staged `build-deps/sqlite`). The C code is
      identical either way; swap to a committed amalgamation when the network allows.
- [ ] **Naming convention** for the rebranded symbols (`cel_*` / `CEL_*`?).

---

## Notes / context

- pgforge's old backlog and the multi-app rationale live in the source project; the key
  conceptual writeups to carry forward are `MULTI_APP.md` and `VISION.md` (the
  mechanism/policy decision rule).
- portico-side companion work (domain → app routing via a config file + SIGHUP reload)
  is sketched in `portico/docs/portico-config-design.md` — cellar is the engine that
  config would point each domain at.
