# Multi-App on One pgforge — Exploration

> **STATUS: 🟡 CONSIDERING / FUTURE POSSIBILITY — NOT A COMMITMENT.**
> This is a captured design conversation, not a plan of record. No code has been
> written or changed. It records *one* candidate shape for running multiple apps in a
> single pgforge process, why it fits the existing architecture, and where the work
> would land **if** we decided to do it. Numbers, file references, and signatures are
> indicative — verify against current code before acting.
>
> **Open thread:** multi-tenancy correctness is being revisited separately before any
> of this is firmed up. Treat the tenancy mechanics described here as "as currently
> built," pending that review. See *Open questions* at the end.

---

## Where we are today

```
1 pgforge process + 1 Postgres = 1 APP
   globals: one catalog (g_active), one connection pool, one policy bundle,
            one role config, one tenant-column setting
   └─ optionally MULTI-TENANT  (PGF_TENANT_COLUMN → RLS on rows + app-level scope)
```

Everything app-shaped is a **process-global singleton**:
- `schema_catalog.c` — `g_active` catalog, reached via `pgf_catalog_active()`.
- `db_connection.c` — one global pool (no handle passed around).
- `make_scope` (api.c) — reads the single global `pgf_tenancy_column()`.
- one `config/policies.json` policy + role bundle.

So pgforge is single-app today, and single-app can be single- *or* multi-tenant.

## Where we want to get to

```
1 pgforge process + 1 Postgres = N APPS  (a registry of app bundles)
   ├─ app A → MULTI-TENANT   (RLS on rows)
   └─ app B → SINGLE-TENANT
```

One process, one Postgres, many apps; **each app independently configured as
single- or multi-tenant.**

---

## The keystone (same discipline we've used twice)

This is the *third* instance of a pattern already in the codebase:
- "single-tenant is the degenerate case of multi-tenant" (Phase 7)
- "staff is the degenerate case of end-users" (Phase 8)
- → **"single-app is the degenerate case of multi-app."**

Today's globals become a **per-app bundle held in a registry**, and a single
**app-resolution step** is added at the very front of the request. Single-app = a
registry with one entry. No `if(multi_app)` branching.

## The shape: an app is a Postgres schema

```
one Postgres database
 ├─ schema app_a   →  pgf_users, pgf_sessions, pgf_identities, products, orders …  (+ RLS tenant_id)
 └─ schema app_b   →  pgf_users, pgf_sessions, menu …                              (no RLS)
```

Why schema-per-app (vs. an `app_id` discriminator column, or a database/instance per app):
- **Identity isolation falls out for free.** Each app's `pgf_users`/`pgf_sessions`/
  `pgf_identities` live in its own schema; a session token only resolves inside its
  own app. Structural, not a check to remember.
- **One shared pool stays one pool.** App selection is per-*request* (`SET LOCAL
  search_path`), not per-connection — so no pool fan-out.
- **Composes with tenancy** (next section) instead of fighting it.

## Two composable `SET LOCAL`s

App isolation is tenancy's mechanism one level up. The run-path chokepoint
(`run_rows` in api.c, which already wraps `BEGIN; set_config(tenant); query; COMMIT`)
gains exactly one statement:

```
BEGIN;
  SET LOCAL search_path = "app_a";                      -- NEW: app picks the TABLES
  SELECT set_config('app.tenant_id', $1, true);         -- EXISTING: tenant picks the ROWS (multi-tenant apps only)
  <query>;
COMMIT;
```

`SET LOCAL` is transaction-scoped → auto-resets at COMMIT → **no leak across pooled
checkouts** (same property the tenant `set_config` already relies on). The two axes
are orthogonal:
- **App isolation** = schema / `search_path` (which *tables* exist)
- **Tenant isolation** = RLS / `app.tenant_id` (which *rows* you see)

`single-app` and `single-tenant` are both the degenerate case: a registry of one, and
`tenant_column = NULL`.

---

## Where the changes would land

### The spine — implicit global context → explicit app context (wide but shallow)

| Today (global) | Becomes | Where |
|---|---|---|
| `pgf_catalog_active()` (api.c:39, 383, 814; http_routes.c:227) | `app->catalog` | schema_catalog, api.c, http_routes |
| `pgf_tenancy_column()` (api.c:264) | `app->tenant_column` | api.c `make_scope` |
| `run_rows` / `run_rows_pipelined` (api.c:172, :97) | also thread `app->schema` → emit `SET LOCAL search_path` | api.c — **both** run paths |
| one global pool | **unchanged** — search_path is per-request | — |

Bulk of the work: define `pgf_app_t {catalog, policy, roles, tenant_column, schema,
session_cache}`, thread a `pgf_app_t *app` parameter down `pgf_api_*` → `run_rows` /
`make_scope` instead of reaching for globals. Many signatures, not deep. SQL stays
byte-identical for the single-app case (the `query_builder` golden tests still pass).

### Security-critical scopings (the two spots risk concentrates)

1. **Session cache** (`session_cache.c`) — today a process-global hash keyed by token
   hash. Must become **per-app** (an instance inside each `pgf_app_t`, or key on
   `(app_id, token_hash)`). Otherwise a token cached for A could resolve in B.
2. **Realtime** (`realtime.c` + `pgf_api_authorize_subscription`) — subscriptions and
   the publish fan-out must be **app-scoped**, so a write in A only reaches A's
   subscribers. The fd→app binding is set once at auth.

### New subsystem — app registry + config

- **Registry loader** at startup: enumerate apps (a `config/apps/<id>/` dir, or a
  control table `pgf_apps(id, schema, tenant_column, …)`), introspect **each schema's**
  catalog, load **each** policy/role bundle, build the registry.
- **App-resolution step** at the front of the request (`http_routes.c` router +
  `main.c on_binary_message`): resolve a selector → `pgf_app_t *` → pass down. The
  selector is still an open decision (Host header / path prefix / app-id header).
- **Per-schema migrations** (`migrate.c`): `CREATE SCHEMA app_x`; run the core
  migrations (users/sessions/identities/mfa/…) into it; `tenancy-protect` per
  multi-tenant schema.

### Falls out for free

- **Per-app tenancy.** `make_scope` already appends the tenant rule *conditionally* on
  a non-NULL tenant column (api.c:73, :264). Point it at `app->tenant_column` and "app
  A multi-tenant, app B single" works with **zero new logic** — single-tenant is
  already the degenerate path.

---

## Isolation / leakage analysis (apps are *ours*, but defend against accidents)

The worry is **accidental/logical** cross-app leakage (a policy/scope bug, a wrong
`search_path`, a mis-keyed cache), not an attacker escaping a sandbox. The dividing
line that matters:

> **Is the boundary enforced by Postgres, or does it rely on pgforge being bug-free?**

The leakage ladder — every realistic way A's data reaches B, and what closes it:

| # | Vector | Closed by |
|---|---|---|
| 1 | pgforge policy/scope/catalog **bug** → cross-table read | **role/grant per schema** (Postgres refuses) |
| 2 | wrong `search_path` / wrong target | role-per-schema (no `USAGE` on others) + per-request `SET LOCAL` |
| 3 | shared session cache keyed by token only | **per-app session cache** |
| 4 | shared connection-pool reuse acting as wrong app | per-request `SET LOCAL` reset (txn-scoped) |
| 5 | **superuser** DB role bypasses RLS + grants | non-superuser role (already required for RLS) |
| 6 | `dblink` / FDW cross-DB | don't grant it |
| 7 | Postgres process memory bug / RCE (shared instance) | separate instance — *not* "leakage," an exploit |
| 8 | kernel exploit | microVM — overkill for own apps |

**Punchline:** vectors 1–6 — every realistic accidental-leak path — are closed
*without* a second Postgres instance, by:
- per-request `SET LOCAL search_path` (+ tenant `set_config` for multi-tenant apps),
- a **non-superuser role with `USAGE`/grants scoped to its own schema** (converts "the
  engine must be bug-free" into "Postgres rejects it at the privilege layer"),
- a **per-app session cache** and **app-scoped realtime**,
- no `dblink`, no superuser.

**Confidentiality vs. availability.** The above gives *confidentiality* isolation on a
shared instance. What a shared instance can **not** give: *availability* isolation —
one postmaster means a runaway query / OOM / connection-storm in app A can starve B
(shared fate). Mitigate cheaply with per-role `statement_timeout` + connection limits
(+ systemd `CPUQuota`/`MemoryMax`); if hard availability isolation is ever needed for a
specific app, graduate *that app* to its own Postgres instance (or microVM). **Isolation
can be decided per-app — no single global answer is owed up front.**

---

## The invariant to hold (if we build this)

Mirroring "every auth method converges on the opaque `pgf_sessions` token":

> **Every request resolves to exactly one `(app, [tenant])` context before any data
> access, and that context is pinned on the connection via `SET LOCAL` inside the
> request's transaction.**

Hold that and isolation is structural rather than disciplinary.

---

## Open questions (deferred — revisit before committing)

1. **Multi-tenancy correctness** — the prerequisite. Confirm the *current* tenancy
   model (app-level `pgf_scope_t` + RLS seatbelt + `set_config('app.tenant_id')`) is
   right before stacking app-isolation on top of it. **This is the next conversation.**
2. **App-resolution selector** — Host header (vhost, pairs with SNI; needs a Host
   allow-list — never trust Host unchecked) vs. path prefix vs. app-id/API-key header.
3. **Config source** — a `config/apps/<id>/` directory vs. a `pgf_apps` control table
   (the latter enables runtime provisioning).
4. **Runtime provisioning vs. startup-only** — start with load-at-startup; add an
   online "create app" API later (relates to deferred task #55b).
5. **Role granularity** — role-per-schema grant model + how migrations bootstrap it.
6. **Metrics** — add an `app` label, or accept aggregate.

## Magnitude (honest read)

- **Conceptually:** small — tenancy's pattern one level up; two composable `SET LOCAL`s;
  degenerate-case discipline intact.
- **Mechanically:** one wide-but-shallow refactor (thread `pgf_app_t *`) + two careful
  scopings (session cache, realtime) + one new subsystem (registry/loader, per-schema
  migrations, resolution step).
- **Risk concentrates** in exactly two places: the session-cache key and the realtime
  fan-out. Everything else is enforced by `SET LOCAL search_path` + role-per-schema.
