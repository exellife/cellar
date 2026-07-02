# cellar app-bundle contract

What cellar expects of an **app bundle** — the on-disk unit it serves. One bundle =
one app / org / tenant. cellar routes to it by the **Host** header (multi-app mode),
opening it lazily on first request. The engine stays generic; everything
domain-specific lives in the bundle.

Companion docs: [`cellar-design.md`](cellar-design.md) (engine + hook contract §8),
[`policy-guide.md`](policy-guide.md) (authz), [`cellar-sync-design.md`](cellar-sync-design.md)
(offline-first sync), [`frontend-guide.md`](frontend-guide.md) (client).

---

## 1. Layout

```
$CEL_APPS_DIR/                 # parent dir of all bundles (env)
└── <host>/                    # one bundle, named after the routed Host
    ├── data.db                # SQLite: identity (cel_*) + app tables + sync state
    ├── hooks.lua              # behavior: before/after/authorize/resolve/rpc/job/render_email
    ├── policies.json          # authz overrides (optional → built-in role defaults)
    ├── public/                # static front-end root, served at "/" (optional)
    ├── schema.sql             # app schema — a BUILD/DEPLOY input, NOT read at runtime
    └── migrations/            # ordered schema deltas — PROPOSED (see §4)
```

`<host>` is the exact Host cellar matches (e.g. `localhost`, `portico-test.duckdns.org`).

| File | Read by the running server? | Role |
|---|---|---|
| `data.db` | **yes** (the live DB) | identity tables (`cel_*`), app tables, sync state (`_sync_*`) |
| `hooks.lua` | **yes**, at app-open | write-time validation/stamping, sync conflict `resolve()`, rpcs, jobs |
| `policies.json` | **yes**, at app-open | per-table role grants + `realtime`; absent → engine defaults |
| `public/` | **yes**, per request | front-end assets, served at `/` (same-origin with the API) |
| `schema.sql` | **no** | the app's table definitions; applied to `data.db` at deploy time |
| `migrations/` | **no** (run by `cellar migrate`) | forward-only schema deltas for existing DBs |

Only `data.db` is irreplaceable — it holds user data. Everything else comes from git.

---

## 2. Lifecycle (today)

```sh
# 1. provision — create the bundle dir + data.db (identity schema) + seed an admin.
#    Idempotent: re-running keeps existing data.db rows and bundle files.
CEL_APPS_DIR=/path/to/apps  cellar provision <host> admin@example.com <adminpw>

# 2. apply the app schema (operator step — provision does NOT do this)
sqlite3 /path/to/apps/<host>/data.db < schema.sql

# 3. drop in the frontend bundle (overwrites the starter stubs)
cp -r public hooks.lua policies.json  /path/to/apps/<host>/

# 4. run cellar (front with TLS, point <host> DNS at it)
CEL_APPS_DIR=/path/to/apps  cellar
```

**Multi-app facts:**
- Routes by Host; opens each bundle lazily on first request; an unconfigured Host is
  never served (allow-list).
- **No boot seeding** — `CEL_SEED_USERS` is ignored in multi-app mode. Accounts come
  from `provision` (the first admin) + `POST /auth/users` (admin-created), or
  self-register **iff** `policies.json` opens a role to it.
- `hooks.lua`/`policies.json` are read at app-open → **edit + restart cellar** to pick
  up changes (or re-open the app). `public/` is served from disk per request.

---

## 3. Schema (today)

`schema.sql` is a **create-only** script (`CREATE TABLE IF NOT EXISTS …`). It is
idempotent for *creating* tables, so re-running it on a populated DB adds any **new
tables** — but it can **not** evolve an existing table (add a column, change a
constraint). That's the gap §4 fills.

The engine's *own* identity tables (`cel_*`) are already migrated in place: cellar
tracks `PRAGMA user_version` and, when a DB is behind `CEL_AUTH_SCHEMA_VERSION`, runs
`ALTER TABLE …` and bumps the version (`src/core/auth_schema.c`). **`user_version` is
owned by the engine** — a bundle's migration system must use its own tracker, not this.

Syncable app tables carry `rev INTEGER` + `deleted INTEGER` and are mirrored by
clients; see [`cellar-sync-design.md`](cellar-sync-design.md). A schema change to a
syncable table is therefore also a **client**-schema change (the local mirror /
drift schema) — roll out **additive-first**: server leads, clients catch up; renames
and drops are breaking and need coordinated client+server release.

---

## 4. Migrations

To evolve a populated `data.db` without wiping it, the bundle carries ordered,
forward-only deltas and cellar applies the ones a given DB hasn't seen — mirroring
the auth-schema pattern, but with a bundle-owned tracker. **Shipped** as
`cellar migrate <host>` (`src/core/migrate.c`).

```
migrations/
├── 0001_init.sql              # full initial schema (== today's schema.sql)
├── 0002_add_staff_profiles.sql
└── 0003_booking_authorship.sql   # ALTER TABLE bookings ADD COLUMN created_by …
```

- **Tracker:** a `_schema_migrations` table (`_`-prefixed → excluded from the catalog
  like `_sync_seq`). Records each applied migration id (+ time, + checksum). **Not**
  `user_version` (engine-owned).
- **`cellar migrate <host>`** subcommand: takes a consistent `VACUUM INTO` backup to
  `<bundle>/.backups/pre-migrate-<ts>.db` first → applies each pending migration in its
  own transaction (atomic with the bookkeeping insert) → records it with a checksum.
  Idempotent, per-app, **explicit** (you run it; it does not fire silently on app-open).
  An already-applied file whose checksum changed is a hard error (drift detection).
  **Run it with the app stopped** — a schema change (`ALTER`) takes a write lock and
  must not contend with a live, writing server. The `dist/cellar-migrate.sh` wrapper
  enforces stop → migrate → start (and restarts the service even if the migration fails).
- **`schema.sql` stays** as the fast fresh-install path; long-term it becomes a
  *generated snapshot* of the migration end-state (the Rails model — migrations are
  truth, the snapshot is for speed + readability).
- **Hard SQLite changes** (type/constraint changes, dropping a column pre-3.35, adding
  a NOT NULL column without default) need the hand-written 12-step table-rebuild
  inside the migration file — cellar runs the SQL, it doesn't diff schemas.

Forward-only + a pre-migrate backup (below) is the rollback story; no down-migrations.

**Authoring (bundle owners).** Migrations are bundle content — whoever owns the
schema writes them. Rules: one file per change, `NNNN_` monotonic; **never edit an
applied file** (checksummed → drift is rejected; fix-forward with a new file); SQL
only, no `BEGIN`/`COMMIT`. The **first** migration is special — to adopt an
*already-deployed* DB, `0001_init.sql` must be the current schema with `CREATE … IF
NOT EXISTS` on every object, so `migrate` is a full create on a fresh DB and a no-op
that just records the baseline on an existing one. Every later migration is plain
forward SQL (`ALTER TABLE … ADD COLUMN …`; hard changes → the 12-step rebuild).

---

## 5. Backups & data safety (today)

- **`cellar export <host> <out.tar.gz>`** — a consistent single-file snapshot of
  `data.db` via `VACUUM INTO` (safe on a live DB), tarred with the bundle.
- **`cellar import <host> <in.tar.gz>`** — restore it.
- **Discipline:** a scheduled `cellar export` (systemd timer / cron, rotated, ideally
  off-box) is the real "never lose data" floor — it covers corruption and operator
  error, not just deploys. Always `export` immediately **before** a deploy/migrate so
  rollback = `import` the pre-deploy snapshot.

---

## 6. Deploy contract (what a build must hand cellar)

A self-contained bundle dir (the "frontend-side" deliverable), ready to drop on the host:

- `public/` — production build, **same-origin** (client `baseUrl=""` → relative
  `/auth` `/sync` `/rpc`, no CORS; the bundle is portable across domains).
- `schema.sql` + `migrations/` (§4), `hooks.lua`, `policies.json`.
- **No `data.db`** — the server creates it (`provision`).

Each deploy, state the server-affecting delta: (a) did the schema touch *existing*
tables (→ migrate) or only add tables (→ in-place); (b) any new role names in
`policies.json` (so the right users get seeded); (c) does `hooks.lua` use an engine
feature newer than the target's cellar binary (→ rebuild cellar there). And the login
UI must match the policy (e.g. if self-register is off, no "create account" path).

**Redeploy loop** (existing app):
`export` backup → rsync `bundle/` → copy `public/`+`hooks.lua`+`policies.json` →
**stop cellar → `cellar migrate <host>` → start cellar** → smoke-test (a sync
round-trip + a login). The stop→migrate→start middle is what `dist/cellar-migrate.sh`
does (a schema change takes a write lock — don't migrate under a live writer).
First deploy of a *new* app: `provision` + `cellar migrate` instead of the stop/start.
Rollback → `import` the pre-deploy snapshot (or restore `<bundle>/.backups/`).
