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
- [ ] **Design doc** (`docs/cellar-design.md`, in the style of
      `portico/docs/portico-config-design.md`): the SQLite multi-app model — the
      blocking-vs-event-loop concurrency contract, per-app handle cache + per-app write
      serialization, per-app catalog, the auth fork (per-app users vs a shared
      control-plane DB), and the ports-vs-rewrites map. Lock the direction before code.

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
      `.db` file. A small **control-plane DB** (app registry, domain→file routing,
      platform admin). Provision = create file + migrate; delete = `rm`; export = `cp`.

## Open decisions (resolve in the design doc)

- [ ] **Auth model:** self-contained per-app users, a shared control-plane identity, or
      the hybrid (control-plane registry + per-app users).
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
