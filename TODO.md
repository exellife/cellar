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

- [ ] **Baseline commit.** Commit the verbatim pgforge copy ("fork from pgforge") so the
      rebrand + re-engine diffs are clean and reviewable.
- [ ] **Rebrand `pgforge → cellar`** (~53 files). Source identifiers (`pgf_*` →
      `cel_*`?), env vars (`PGF_*` → `CEL_*`), CMake project + binary target, README,
      `dist/pgforge.service` → `cellar.service`. Goal: compiles and runs cleanly under
      its own name, still Postgres-backed — pure rename, no behavior change yet.
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

- [ ] **`db_connection.c` → per-app SQLite layer.** Open-by-file, an LRU cache of
      `sqlite3*` handles, WAL mode, and a **per-app write queue** so one write-heavy app
      can't starve others (SQLite is single-writer per file; never touch the event
      thread — every call runs on the worker pool).
- [ ] **`schema_catalog.c` → PRAGMA introspection.** `sqlite_master` +
      `PRAGMA table_info / foreign_key_list / index_list` instead of `information_schema`;
      the catalog becomes **per-app** (replaces today's single global `g_active`).
- [ ] **`query_builder.c` → SQLite dialect.** `?`/`:name` placeholders (not `$N`),
      `RETURNING` (SQLite ≥ 3.35), type-affinity handling for JSON serialization.
- [ ] **Delete the tenant machinery.** No `tenant_id` column, no `set_config` /
      `SET LOCAL`, no RLS — isolation is the file boundary. Removes `run_rows`' tenant
      context entirely.
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
- [ ] **Naming convention** for the rebranded symbols (`cel_*` / `CEL_*`?).

---

## Notes / context

- pgforge's old backlog and the multi-app rationale live in the source project; the key
  conceptual writeups to carry forward are `MULTI_APP.md` and `VISION.md` (the
  mechanism/policy decision rule).
- portico-side companion work (domain → app routing via a config file + SIGHUP reload)
  is sketched in `portico/docs/portico-config-design.md` — cellar is the engine that
  config would point each domain at.
