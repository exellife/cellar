# pgforge — Design Plan

A Supabase-style, schema-driven backend for admin dashboards (manage products, categories, etc.),
built in C on PostgreSQL, installable on any Linux box as a single service.

Derived from the existing `voter-registry-backend`, which already solves the hard
plumbing (WebSocket transport, connection pool, auth, sessions, pub/sub, clean
layering). The job is to **replace per-entity hardcoded handlers with a generic,
schema-driven CRUD engine** — the way Supabase/PostgREST do.

---

## 1. Core idea

| Today (voter-registry) | Target (generic backend) |
|---|---|
| ~60 opcodes, one handler per entity-op (`OP_CREATE_VOTER`, `OP_SEARCH_VOTERS`…) | ~8 generic opcodes (`CREATE/READ/UPDATE/DELETE/LIST/QUERY/RPC/SCHEMA`) |
| SQL hand-written per table in repositories | SQL **generated** from a schema catalog + a query builder |
| Structs per entity, manual `PQgetvalue` column copying | Generic row → JSON serializer driven by column metadata |
| Auth rules baked into each handler | Declarative **policy engine** (row/role rules) consulted generically |
| Schema baked into code | Schema discovered from `information_schema` at startup (+ live reload) |

The platform layers (`wslib`, `opcode_dispatcher`, connection pool, `session_manager`,
`topic_router`/pubsub, `servermesh`, `logger`, Argon2/JWT auth) are **reused almost
unchanged**.

---

## 2. Reuse vs. build

**Reuse as-is**
- `lib/wslib` — WebSocket transport
- `lib/opcode_dispatcher` — routing + thread pools (CPU/DB)
- `lib/servermesh` — multi-instance clustering
- `lib/logger` — logging
- `src/core/db_connection.*` — libpq connection pool
- `src/core/session_manager.*` — fd → {user, role, token}
- `src/core/topic_router.*` — pub/sub (becomes generic realtime)
- Auth primitives — Argon2id (libsodium), JWT, sessions table

**Build new (the engine)**
1. **Schema catalog** — introspect Postgres, hold table/column/PK/FK/type metadata in memory.
2. **Query builder** — compose parametrized SQL (always `PQexecParams`, never string interpolation).
3. **Generic CRUD handlers** — the ~8 opcodes below.
4. **Generic JSON (de)serializer** — row ↔ JSON using catalog types (cJSON).
5. **Policy / authorization engine** — declarative per-table rules (this is the security core).
6. **Migration runner** — apply ordered `.sql` files (reuse the existing migration convention).
7. **Admin UI** — auto-generated tables/forms from the catalog (static assets served by the server).

**Decided**
- Transport: **REST/HTTP front door + WebSocket for realtime** (see §3a).
- Build approach: **fresh project in `pgforge/`, pulling proven pieces from
  voter-registry deliberately** (vendor `lib/*`, copy core/auth/pool/pubsub as needed).

---

## 3. Generic protocol (data-layer opcodes)

Replace entity opcodes with a small fixed set. Payload is JSON; the engine resolves
the table from the catalog.

```
0xD0 DB_LIST     { "table": "products", "select": ["id","name","price"],
                   "where": {"category_id": {"eq": 7}}, "order": ["-created_at"],
                   "limit": 50, "offset": 0 }
0xD1 DB_GET      { "table": "products", "id": 42 }
0xD2 DB_CREATE   { "table": "products", "values": {"name":"X","price":9.99} }
0xD3 DB_UPDATE   { "table": "products", "id": 42, "values": {"price": 8.50} }
0xD4 DB_DELETE   { "table": "products", "id": 42 }
0xD5 DB_QUERY    { "table": "products", filter/aggregate/join DSL }  // advanced reads
0xD6 DB_RPC      { "fn": "some_postgres_function", "args": {...} }   // escape hatch
0xD7 DB_SCHEMA   { }  // returns catalog so the admin UI can render itself
```

Realtime stays on the existing pub/sub: a successful write publishes to
`table:<name>` (and optionally `table:<name>:row:<id>`); subscribers get change events.

## 3a. REST front door (chosen transport)

A thin HTTP layer is the primary client interface; WebSocket carries realtime. Both
funnel into the **same generic engine** — HTTP handlers just translate request →
internal CRUD call → JSON response. No duplicated logic.

```
GET    /api/<table>            → DB_LIST    (querystring: select, where, order, limit, offset)
GET    /api/<table>/:id        → DB_GET
POST   /api/<table>            → DB_CREATE  (JSON body = values)
PATCH  /api/<table>/:id        → DB_UPDATE  (JSON body = values)
DELETE /api/<table>/:id        → DB_DELETE
POST   /rpc/<fn>               → DB_RPC
GET    /schema                 → DB_SCHEMA
POST   /auth/login             → issue JWT
GET    /admin/*                → static admin SPA
WS     /realtime               → subscribe to table:<name> change events
```

Auth via `Authorization: Bearer <jwt>`; the same session/role lookup feeds the policy
engine. An HTTP server lib is needed (candidates: a minimal one atop wslib's socket
layer, or mongoose / libmicrohttpd) — pick during Phase 2.5.

### Request flow
```
WS frame → wslib → opcode_dispatcher (DB pool)
   → generic CRUD handler
       1. auth: session_manager_get(fd) → who is this?
       2. resolve table in schema catalog (404 if unknown)
       3. policy engine: is this user allowed this op on this table/rows?
       4. query builder: build parametrized SQL (+ inject policy WHERE clauses)
       5. db_connection_acquire() → PQexecParams → release
       6. serialize PGresult → JSON via catalog
       7. send_response(fd, ...) ; on write, topic_router publish change event
```

---

## 4. The schema catalog (heart of the engine)

At startup (and on `DB_SCHEMA reload`), query:
- `information_schema.tables` / `columns` — names, types, nullability, defaults
- `table_constraints` + `key_column_usage` — primary keys, foreign keys, uniques
- optionally `pg_enum` for enum value lists (great for UI dropdowns)

Hold an in-memory model:
```c
typedef struct { char name[64]; pg_type_t type; bool nullable, is_pk, is_fk;
                 char fk_table[64], fk_col[64]; } column_meta_t;
typedef struct { char name[64]; int n_cols; column_meta_t cols[MAX_COLS];
                 const column_meta_t *pk; } table_meta_t;
typedef struct { int n_tables; table_meta_t tables[MAX_TABLES]; } schema_catalog_t;
```
This drives: which tables are exposed, type-correct SQL binding, JSON typing, FK
resolution for the UI, and the auto-generated forms.

**Type mapping** (Postgres → C bind → JSON):
int*/numeric → number; bool → bool; text/varchar/uuid/timestamptz → string;
json/jsonb → embedded JSON; arrays → JSON array. A small type table centralizes this.

---

## 5. Authorization — the part you cannot skip

Generic CRUD over arbitrary tables is dangerous without rules. Supabase solves this
with Postgres RLS; we implement a lighter **app-level policy engine** (optionally
backed by real RLS later).

**Policy config** (e.g. `config/policies.json`), per table per action:
```json
{
  "products": {
    "list":   { "roles": ["admin","editor","viewer"] },
    "get":    { "roles": ["admin","editor","viewer"] },
    "create": { "roles": ["admin","editor"] },
    "update": { "roles": ["admin","editor"], "row": "owner_id = :user_id" },
    "delete": { "roles": ["admin"] }
  },
  "_default": { "deny": true }        // optional; deny IS the default — unlisted tables
}                                      // are denied unless "_default":"allow" (or {allow:true})
```
> **Fail-closed:** when a policy file is loaded, a table with no entry is **denied** by
> default. Set `"_default": "allow"` (string) or `{"allow": true}` / `{"deny": false}`
> (object) to opt back into the permissive built-in/`_roles` fallback for unlisted tables.
> Both the string and object spellings of `_default` are accepted; omitting it logs a warning.
- **Role check** gates the op (role comes from `session_manager`).
- **Row predicate** (`row`) is injected as an extra parametrized `WHERE` clause, so
  users only ever touch rows they own / are scoped to (multi-tenant by `tenant_id`,
  ownership by `owner_id`, etc.).
- **Column allow/deny** lists can hide sensitive columns from `select`/`values`.
- **Deny-by-default**: a table with no policy is not reachable. This is the single
  most important safety rule.

Auth issuance (login → Argon2id verify → JWT + session) is reused verbatim.

---

## 6. Admin UI (auto-generated)

`DB_SCHEMA` returns the catalog + policies-visible-to-this-user; a small static
SPA (served by the server, no separate process) renders:
- a sidebar of tables, a data grid per table (paginated via `DB_LIST`),
- create/edit forms generated from column metadata (types → input widgets,
  enums → dropdowns, FKs → reference pickers),
- delete with confirm.

Ships as static files embedded in / served beside the binary, so "install" stays
one artifact. This is the PocketBase-style admin experience.

---

## 7. Phased roadmap

**Phase 0 — Stand up a fresh platform. ✅ DONE.** Skeleton in `pgforge/`; vendored
`lib/{wslib,opcode_dispatcher,logger}` (mesh dropped — not needed yet); vendored
`db_connection`; wrote fresh generic auth (Argon2id via libsodium + opaque
server-side tokens in `pgf_users`/`pgf_sessions`) rather than dragging in the
voter-specific auth service. `main.c` boots dispatcher (CPU+DB pools) + DB pool +
WebSocket listener. *Exit met: one `make` builds standalone; `scripts/smoke_test.py`
shows PING/ECHO/SERVER_INFO + LOGIN/VERIFY/LOGOUT all pass.* (In-memory per-connection
session manager deferred to Phase 4, where the policy engine needs per-fd identity.)

Add **Phase 2.5 — REST front door**: choose/integrate the HTTP lib and map the routes
in §3a onto the engine, once the read path (Phase 2) exists.

**Phase 1 — Schema catalog. ✅ DONE.** `src/engine/schema_catalog.{c,h}` introspects
`information_schema` (columns, normalized types, PKs, FKs) into an in-memory model,
excludes internal `pgf_*` tables, and serves it as JSON via `OP_DB_SCHEMA` (0xD7).
Demo schema in `002_demo_products.sql`. *Exit met: smoke test asserts products/
categories, PK, type normalization, and the category_id→categories FK.* Note: catalog
is built once at startup; live reload needs an RWlock (deferred).

**Phase 2 — Read path. ✅ DONE.** `engine/query_builder.{c,h}` builds parametrized
SELECTs (catalog-validated identifiers + `$N` binds only — injection-safe by
construction); `engine/row_json.{c,h}` serializes `PGresult` to typed JSON via the
catalog. `OP_DB_LIST` (0xD0: select/where/order/limit/offset, where-DSL ops
eq/neq/lt/lte/gt/gte/like/ilike/in + null) and `OP_DB_GET` (0xD1, by PK) on POOL_DB.
Shared `handlers/respond.{c,h}`. *Exit met: smoke test reads real rows with
filter/order/pagination and proves both injection vectors (identifier + value) are
blocked.* where-clauses are ANDed; limit defaults 100 / caps 1000.

**Phase 2.5 — REST front door. ✅ DONE.** Extracted the transport into a standalone
library, **portico** (`../portico`, on GitHub at exellife/portico) = hardened wslib +
an HTTP/1.1 layer on picohttpparser, one listener serving both. pgforge now consumes
portico (replacing vendored `lib/wslib`). Engine made transport-neutral
(`src/engine/api.{c,h}`: `pgf_api_list/get/schema/login` returning `{body, http_status}`);
WS opcode handlers and the new REST router (`src/handlers/http_routes.c`) are both thin
adapters over it. Routes: `GET /schema`, `GET /api/<table>` (querystring
select/order/limit/offset + `<col>=<op>.<val>` filters, PostgREST-ish), `GET /api/<table>/<id>`,
`POST /auth/login`. *Exit met: `tests/rest_test.py` (CTest `rest`) green incl. identifier
injection→400 and value injection neutralized.*

**Phase 3 — Write path. ✅ DONE.** `pgf_build_create/update/delete` (parametrized
INSERT/UPDATE/DELETE … RETURNING; catalog-validated columns; jsonb values serialized);
`pgf_api_create/update/delete`. Postgres SQLSTATE → HTTP mapping (unique→409, fk→409,
not-null/check/data-exception→400, incl. the bad-UUID 22P02→400 refinement). WS opcodes
`OP_DB_CREATE/UPDATE/DELETE` (0xD2–0xD4) + REST `POST /api/<table>`,
`PATCH|PUT /api/<table>/<id>`, `DELETE /api/<table>/<id>`. *Exit met: full CRUD lifecycle
green on both transports (smoke WS opcodes + rest), self-cleaning, with 409/400 constraint
mapping.* Realtime change events (publish to pub/sub on write) still deferred — topic_router
isn't wired into pgforge yet (Phase 0 deferral).

**Phase 4 — Policy engine. ✅ DONE (role-based).** `src/engine/policy.{c,h}`:
`pgf_identity_from_token` (token→identity via `pgf_auth_verify`) + `pgf_policy_allows`
(deny-by-default, role-per-action; admin=all, editor=read+create+update, viewer=read,
anon=nothing). Enforced inside every `pgf_api_*` (unauth→401, wrong role→403). Identity
resolved uniformly: REST `Authorization: Bearer`, WS `token` field in payload (schema
moved to POOL_DB). Optional `PGF_POLICY_FILE` JSON overrides per table. Multi-role seeding
via `PGF_SEED_USERS="email:pass:role;..."`. *Exit met: smoke (WS) + rest both green incl.
anon→401, viewer create→403, editor delete→403, admin all.*

**Phase 4b — Row-level ownership. ✅ DONE.** Policy actions may be `{roles, owner_column}`;
`pgf_policy_owner_column` + a `pgf_ownership_t` threaded through the builders inject a
parametrized `AND owner_column = $N` (= caller's user id) into list/get/update/delete, and
INSERT **forces** the owner column to the caller (client-supplied owner ignored; updating it
is rejected). admin bypasses (superuser). Demo `notes` table + `config/policies.json`
(`PGF_POLICY_FILE`); tables not listed fall back to role defaults. *Exit met: rest test —
owner isolation across two editors (get/list/patch/delete→404 for non-owner), owner-spoofing
ignored, owner-reassign→400, admin bypass.* **Still deferred:** column-level rules
(hide/deny specific columns per role); tenant-scoping by a `tenant_id` in the identity
(currently ownership is by user id only).

**Phase 5 — Admin UI. ✅ DONE.** `web/` is a petite-vue SPA (no build step, vendored
`web/vendor/petite-vue.iife.js`) driven entirely by `GET /schema` + `/api/*`: login,
sidebar of tables, data grid, create/edit drawer (inputs generated from column
type/nullability/default), delete. Embedded into the binary via `cmake/embed_assets.cmake`
(generates `web_assets.c` byte arrays) and served by the router (`GET /` and assets, after
the API routes). Headless render test in `tests/ui/ui_test.mjs` (jsdom, dev-only) — CTest
`admin_ui`. *Exit met: full schema-driven CRUD from a browser, zero per-table UI code.*
Gotcha fixed: `<template v-if>` breaks petite-vue (detached fragment → null parentElement);
use `<div v-if>`. Note: static bodies are copied per request (no zero-copy yet).

**Phase 6 — Packaging. ✅ DONE (migration runner + self-contained build + systemd).**
- **Migration runner:** migrations embedded in the binary (`cmake/embed_migrations.cmake`);
  `pgforge migrate [status]` applies pending in order, transactional, tracked in
  `pgf_migrations` (idempotent); `PGF_AUTO_MIGRATE=1` on boot; systemd `ExecStartPre`.
  CTest `migrate` (fresh-DB bootstrap + idempotent). → pgforge bootstraps an empty DB itself.
- **Self-contained build:** cJSON vendored (single-file, MIT — no libcjson dep);
  `-DPGFORGE_STATIC=ON` statically links libsodium/libuuid/cJSON, leaving **libpq5 + glibc**
  as the only runtime deps. Truly zero-dep needs a from-source minimal libpq
  (`--without-gssapi --without-ldap`) — deferred.
- **Deploy:** `dist/pgforge.service` (hardened unit) + `dist/pgforge.env.example`; CMake
  `install` rules (`cmake --install`); README Deploy section.
*Deferred:* `install.sh`/`.deb`/tarball (user chose systemd+config only for now);
from-source static libpq.

**Later:** `DB_QUERY` (joins/aggregates), `DB_RPC`, real Postgres RLS option,
file storage, full-text search, REST front door (if chosen in §8).

---

## 7b. Phase 7 — Multi-tenancy (support **both** A and B from one engine)

**Goal.** Let pgforge serve *both* deployment models without forking into two
products:
- **Model A — self-hosted / isolated.** One business runs its own pgforge; the
  whole DB is theirs. (What pgforge is today.)
- **Model B — pooled SaaS.** One deployment serves many businesses (tenants) that
  share tables, scoped by `tenant_id` + Postgres RLS.
- **Hybrid / graduate.** A big or sensitive pooled tenant can be moved to its own
  instance — which is just Model A, provisioned by the platform.

**Key insight — A is the degenerate case of B.** Single-tenant = multi-tenant with
scoping turned off. We already have the machinery: Phase 4b row-ownership injects a
parametrized `WHERE owner = $caller` and forces the column on insert. Tenant scoping
is the *same mechanism at a coarser grain*. So we **generalize one primitive**, we do
not add a parallel code path. The discipline: **no `if (multi_tenant)` branches in the
engine** — single-tenant is just an empty/owner-only rule set.

### Design (the pivotal abstraction — lock before coding)

**1. Scope rules (generalize ownership).** Today the engine passes a single
`pgf_ownership_t { column, value }` to the query builder. Replace it with a small
*list*:

```c
typedef struct { const char *column; const char *value; } pgf_scope_rule_t;
typedef struct { pgf_scope_rule_t rule[PGF_MAX_SCOPE]; int count; } pgf_scope_t;  /* AND-ed */
```

- owner scoping  = one rule `{owner_column, caller.user_id}`
- tenant scoping = one rule `{tenant_column, caller.tenant_id}`
- both          = two rules → `WHERE tenant_id = $1 AND owner_id = $2`
- single-tenant  = owner-only (or empty)

The query builder loops the rules instead of handling one: AND each as a parametrized
predicate on LIST/GET/UPDATE/DELETE; force *every* scoped column on CREATE; reject any
client attempt to set/modify a scoped column. (`is_owner_col` → `is_scoped_col`.)

**2. Two scope sources, composed.**
- **Owner scope** — explicit, per-table, per-action, via `owner_column` in
  `policies.json` (unchanged). Scopes by `user_id`.
- **Tenant scope** — *convention + deployment mode*, not per-table config. A global
  setting `"tenancy": { "enabled": true, "column": "tenant_id" }`. When enabled, **any
  table that has that column** is auto-scoped by the caller's `tenant_id` (all actions),
  discovered from the schema catalog. `enabled:false` ⇒ Model A, zero tenant scoping.

**3. Identity carries tenant.** Add `char tenant_id[37]` to `pgf_identity_t`
(empty/`""` in single-tenant). Resolved from the user record on both transports
(REST Bearer, WS token). `make_scope(table, action, who)` assembles the rule list from
(tenancy mode + table columns) and (policy owner_column).

**4. Two admin tiers (pooled mode) + bypass semantics.** One deployment carries both:

| Actor | `role` | `tenant_id` | Sees |
|-------|--------|-------------|------|
| Platform operator | `platform_admin` | NULL | **all tenants**; manages the `tenants` registry, provisions/suspends |
| Tenant admin | `admin` | their tenant | **all rows in their tenant** (owner scope dropped), that tenant only |
| Tenant user | `editor`/`viewer` | their tenant | **own rows** in their tenant (owner + tenant scope both apply) |

- One `pgf_users` table holds everyone; the tier is just **`role` + `tenant_id`**
  (platform-admin = `platform_admin` + NULL tenant). One login flow.
- **Bypass rules:** *owner* scope is dropped for any admin (as today); *tenant* scope
  is dropped **only** for `platform_admin`. So `platform_admin` is the sole role that
  crosses tenants — the rule-list engine gets this for free, the logic is just role-aware.
- **Bootstrap boundary (security-critical):** platform-admins are created **out-of-band
  only** (CLI/migration, e.g. `pgforge create-platform-admin`) — *never* via tenant
  signup. Tenant signup/provisioning creates only tenant-scoped users; a tenant-admin may
  create more admins **within their own tenant**. This boundary is what stops a tenant from
  escalating to platform. (Tasks #45/#47.)
- **Mode-dependence:** with tenancy **off** (Model A) there is one implicit tenant, so
  `admin` stays a plain global superuser — no platform/tenant split, exactly like today.
  The split only appears with tenancy **on**.

**5. RLS as defense-in-depth (pooled only). ✅ DONE (#46).** App-level scope rules
(1–4) are the primary guard; Postgres RLS is the seatbelt — even a hand-written/buggy
query can't cross tenants. As shipped:
- **Policy** (`pgforge tenancy-protect`, idempotent, re-run after adding tenant tables):
  enables RLS + `FORCE` + `pgf_tenant_isolation` on every public table carrying the
  tenant column. The policy is **fail-closed**:
  `USING (current_setting('app.tenant_id', true) = '*' OR <col>::text = current_setting('app.tenant_id', true))`
  — an unset GUC (`current_setting → NULL`) matches no rows; the platform admin binds
  the `'*'` sentinel to see all tenants.
- **Per-request binding** (in `run_rows`): the **pooling gotcha** resolved by
  `BEGIN → SELECT set_config('app.tenant_id', $1, true) → query → COMMIT/ROLLBACK`.
  `set_config(..., is_local=true)` is transaction-scoped, so it shares the txn with the
  query and resets at COMMIT — it can never leak to the next request reusing the pooled
  connection. Every path ends the txn before release (the pool does not reset state).
- **Deployment requirement:** RLS only bites for a **non-superuser** role — superusers
  (and `BYPASSRLS` roles) skip it. So **run the serving pgforge as a dedicated
  non-superuser DB role** for the seatbelt to engage. (Tests prove RLS via `SET ROLE` to
  a non-superuser; `tests/rls_test.sh`.)

### Task sequence

1. **#41 Design** this abstraction (this section) — done when reviewed.
2. **#42** Generalize the scoping engine: `pgf_ownership_t` → `pgf_scope_t`; builder
   loops rules. Single-tenant behavior byte-for-byte unchanged.
3. **#43** Identity carries optional `tenant_id` (both transports).
4. **#44** Keep single-tenant the default; regression-prove Model A is identical.
5. **#45** Pooled mode: `tenancy` config + `tenants` registry + `tenant_id` on
   `pgf_users`; convention-based per-table scoping.
6. **#46** Postgres RLS + per-request `SET LOCAL` (transaction/pool wrapping).
7. **#47 ✅ DONE.** Tenant lifecycle + admin tiers, via out-of-band CLIs:
   `create-platform-admin`, `create-tenant <name> <admin-email> <pw>` (tenant +
   first tenant-admin), `suspend-tenant`/`resume-tenant` (cascade is_active so a
   suspended tenant's users can't authenticate). `platform_admin` is a policy-layer
   superuser that skips tenant scope; `admin` is the within-tenant superuser.
   Proven end-to-end (`tests/tenant_lifecycle_test.sh`). *Runtime provisioning API
   (platform-admin creates tenants over REST) is a future add — CLI covers bootstrap + ops.*
8. **#48 ✅ DONE.** `pgforge export-tenant <name|id>` writes a loadable SQL script
   (the tenant's users + tenant-scoped business rows, tenant column dropped); load
   it into a fresh `pgforge migrate`'d DB to reproduce that tenant as a standalone
   Model A deployment. Proven by `tests/graduate_test.sh` (round-trip: only that
   tenant's data, no others, target is single-tenant).

**Phase 7 ✅ COMPLETE.** One binary runs single-tenant (A) by default and pooled
multi-tenant (B) by opt-in (`PGF_TENANT_COLUMN`), with two enforcement layers
(app-level scope rules + Postgres RLS), the platform/tenant admin tiers, tenant
provisioning/suspend, and the graduate-to-standalone escape hatch. The whole A/B
bet lived in **#42** (the scope-rule list); B turned out almost entirely additive,
and the `query_builder` regression test proves single-tenant SQL is unchanged.

---

## 7c. Phase 8 — Application backend (end-user identity + access + realtime + RPC)

**Goal.** Turn pgforge from an *internal-data-management* backend into a backend that
configures into **any user-facing business** — a taxi app (riders/drivers), a store
(customers/staff), or a plain internal dashboard — *from one binary, by config, not by
forking*. The data plane is already schema-driven and domain-agnostic; Phase 8 makes the
**identity plane** and the **access plane** equally generic.

**Key insight — staff is the degenerate case of end-users.** Today identity is
staff-shaped: the roles `admin`/`editor`/`viewer` are hardcoded in `policy.c`, users are
provisioned out-of-band, and there is no self-service signup. We generalize exactly the
way Phase 7 generalized scoping (§7b): make the **role vocabulary** and **self-service
identity** *declarative config*, not C constants. `admin`/`editor`/`viewer` become a
*default config bundle*, not built-ins. Discipline (same as Phase 7): **no domain
knowledge in the C core** — no `if (taxi)` branches, roles are opaque strings keyed into
config exactly as table/column names already are. The difference between a taxi backend
and a store backend is a config bundle, not code.

### Design (the pillars — lock before coding)

**1. Declarative roles.** Move role semantics out of `policy.c`
(`default_allows`/`is_superuser_role`) into config: a `roles` block (in `policies.json`
or a sibling, later a Postgres table per §8.2) defines each role's name, whether it is a
superuser, and whether it may self-register. `admin`/`editor`/`viewer` ship as the
default bundle (back-compat; dashboard behavior byte-for-byte unchanged). A taxi config
defines `rider`/`driver`; a store defines `customer`/`staff`. The engine treats roles as
opaque strings — it already does for `platform_admin`.

**2. Self-service identity.** `/auth/register`, **gated by the role config**: only roles
flagged self-registerable may sign up, with config-defined defaults. The **out-of-band
boundary is preserved** — superuser/privileged roles are never self-registerable (the
same principle that stops a tenant escalating to `platform_admin`, §7b). End-users
onboard themselves; staff/admins stay provisioned. Follow-ons: password reset, email
verification, OAuth/MFA, login rate-limiting, account lockout.

**3. Generalized access rules.** The Phase 7 `pgf_scope_t` rule list already covers
per-(table, action, role) `owner_column` — so "rider sees `rider_id = me`, driver sees
`driver_id = me`" works *today* by config. Extend the rule kinds:
- **OR semantics** — one role scoped by *either* of two columns (a single actor who is
  both rider and driver on different trips).
- **Relationship scoping** — "rows where the caller is a participant" via a parametrized
  `EXISTS`/join (membership tables, trip participants). Same primitive (`pgf_scope_t`),
  richer rule kinds; RLS (§7b.5) stays the seatbelt.

**4. Realtime.** Wire the vendored `topic_router` to emit change events
(insert/update/delete) on subscribable topics over the existing WS transport.
Subscriptions are gated by the **same access rules** as reads — *no second authz path*.
Taxi: a per-trip topic (live driver location + trip status). Store: a per-order topic
(status updates).

**5. Domain ops — RPC over SQL functions. (DECIDED, §8.5.)** The reserved `RPC` opcode
(§3) exposes a **whitelist of Postgres functions**, authz'd by role, parameters bound
(injection-safe, exactly like CRUD). Business logic lives in **SQL functions**
(`request_ride`/`accept_ride`/`complete_ride`, `checkout`/`apply_coupon`) — *never in the
C core*, which keeps it domain-agnostic ("configure, don't fork"). Heavy or
external-integrating logic (payments, dispatch/matching) lives in **separate services**
that call pgforge as their data plane.

**6. Scale / runtime.**
- **In-memory session cache** — eliminate the per-request token-verify DB round-trip
  (`pgf_auth_verify` runs a join on *every* authenticated request today); this is the #1
  bottleneck once end-users (not a handful of staff) drive traffic.
- **Connection-pool sizing** guidance; Argon2id login cost is CPU-bound by design
  (acceptable; relevant only under login storms).
- **Runtime provisioning API** — graduate the Phase 7 lifecycle CLIs
  (`create-tenant`, etc.) to online REST ops for `platform_admin`.

### Proof it generalizes — both apps on the same five knobs

| Knob | Taxi/cab | Online store |
|------|----------|--------------|
| **Roles** | `rider`, `driver`, `admin` | `customer`, `staff`, `admin` |
| **Access rules** | `trips`: rider→`rider_id=me`, driver→`driver_id=me` | `orders`: customer→`customer_id=me`; `products`: **anon** read |
| **Signup** | `rider`/`driver` self-register; `admin` out-of-band | `customer` self-registers; `staff`/`admin` out-of-band |
| **RPC** | `request_ride`, `accept_ride`, `complete_ride` | `checkout`, `apply_coupon` |
| **Realtime** | per-trip topic (driver location + status) | per-order topic (status) |

Same engine, different config bundle — the symmetry is the design validating itself.

### Task sequence (draft)

1. **#49 Design** this section — done when reviewed.
2. **#50 ✅ DONE.** Declarative roles: a reserved `_roles` object in the policy config
   defines each role's `superuser` flag and/or default-action `allow` list;
   `policy.c` (`is_superuser_role`/`default_allows`) reads it, falling back to the
   built-ins additively (a role not listed keeps its default; `platform_admin` is always
   a policy-layer superuser). `admin`/`editor`/`viewer` ship as the default bundle, so
   no-config behavior is byte-for-byte unchanged. `config/policies.taxi.example.json`
   shows rider/driver on one binary; DB-free `tests/policy_test.c` (CTest `policy`)
   pins both back-compat and the custom-role bundle. (commit ffd3adc)
3. **#51 ✅ DONE.** Self-service signup: public `POST /auth/register` →
   `pgf_api_register` → `pgf_auth_register` (plain INSERT, duplicate email → 409;
   auto-login returns a session token). Gated by the role config:
   `pgf_role_can_self_register` requires `"self_register": true` AND non-superuser
   (the out-of-band boundary — privileged roles can never be obtained via signup),
   deny-by-default; `pgf_role_default_signup` picks the first self-registerable role
   when the request omits one. Single-tenant only for now (pooled signup needs tenant
   resolution — returns 403 in pooled mode). Min password length 8. Proven by the
   DB-free `policy` test (gating) + the end-to-end `register` CTest (taxi policy:
   rider/driver sign up, admin/editor/unknown rejected, duplicate 409, short pw 400);
   ASan/UBSan clean. 12 CTest suites green.
3b. **#51b ✅ DONE.** Admin-provisioned accounts: authenticated `POST /auth/users` →
   `pgf_api_create_user` → `pgf_auth_create_user` (INSERT, no auto-login; dup → 409).
   Superuser-only; **`platform_admin` can never be minted via any in-band request**
   (CLI/out-of-band only — the platform-tier boundary). Pooled mode: a tenant admin's
   new users are **forced into the admin's own tenant** (client `tenant_id` never
   trusted); `platform_admin` must name the target tenant. Exposed
   `pgf_role_is_superuser` from the policy layer. Closes the user-lifecycle story
   (self-serve signup + admin invite). Proven by the `register` e2e (admin creates a
   driver who can then log in; non-superuser → 403, platform_admin → 403, unauth →
   401, dup → 409) + policy_test superuser cases; ASan clean. (single-tenant e2e;
   pooled tenant-forcing is logic-covered — a pooled e2e is a possible follow-up.)
4. **#52 ✅ DONE.** Generalized access rules atop `pgf_scope_t`: each rule gains a
   `kind` — **EQ** (`col = me`, the base case, byte-for-byte unchanged + still forced
   on INSERT), **OR** (`(colA = me OR colB = me …)` — one caller matches via any
   column), **VIA** (`EXISTS (SELECT 1 FROM rel WHERE rel.ref = t.local AND rel.user =
   me)` — caller is a related participant). OR/VIA are read/filter-only (constrain
   LIST/GET/UPDATE/DELETE, never forced on CREATE). Config: `owner_column` (EQ),
   `owner_any: [...]` (OR), `owner_via: {table,ref,local,user}` (VIA), parsed by
   `pgf_policy_owner_scope`; `make_scope` validates columns against the catalog and
   charset-checks the VIA relationship identifiers (then quoted → injection-safe).
   Proven by byte-exact `query_builder` cases (OR, tenant+OR, VIA list/delete, OR
   not-forced-on-create) + `policy` owner-scope parsing; EQ regression unchanged;
   ASan clean; 12 suites green. taxi example folds rider/driver into one `trips`
   table via OR and shows `trip_messages` via VIA. (Live OR/VIA e2e: possible follow-up.)
5. **#53 ✅ DONE.** Realtime change events (in-process source). `src/engine/realtime.{c,h}`
   is the registry + authz fan-out behind `pgf_realtime_publish(table, op, row)`; a
   demand-gated hook in `run_write` emits after a successful write (skipped unless the
   table is `"realtime": true` *and* someone is subscribed — no per-CRUD tax). WS
   `OP_SUBSCRIBE`/`OP_UNSUBSCRIBE` + a pushed `OP_CHANGE`; `on_disconnect → drop_conn`.
   `pgf_api_authorize_subscription` reuses the LIST access rules: scope rules become
   in-memory delivery predicates (EQ/OR/tenant match the event row directly), and a
   VIA (membership) table requires a keyed subscription `{table, key:{column,value}}`
   whose membership is verified once and collapses to an equality predicate (the chat
   fan-out path — no per-event query). DB-free `realtime` test (predicate matching,
   fan-out, tenant isolation, replace/drop/unsubscribe) + `realtime_e2e` (live WS
   subscribe → write → CHANGE, owner-scoped so it proves the filter); 14 suites green,
   ASan/UBSan clean. **Boundary:** data-change realtime only — ephemeral signals
   (typing/presence) are a separate broadcast feature, deferred; delivery is
   best-effort/at-most-once (clients backfill via LIST on reconnect). (commit 7de158c)
5b. **#53b** LISTEN/NOTIFY source (multi-instance + external writes). A generic trigger
   `pg_notify`s changes on realtime-enabled tables (installed by a `realtime-enable`
   CLI, like `tenancy-protect`); a dedicated LISTEN thread on every instance feeds the
   *same* `pgf_realtime_publish` seam — so the registry, authz, routing, and WS protocol
   are unchanged. The **source becomes a config knob** (`realtime.source = inprocess |
   notify`): `inprocess` (default, zero-setup, demand-gated) → `notify` (every enabled
   write emits, all instances LISTEN, catches external writes). WAL/logical replication
   stays deferred — too heavy an operational contract ("stock Postgres, any Linux box")
   unless a deployment truly needs WAL-grade guarantees.
6. **#54 ✅ DONE.** RPC — domain logic as SQL functions. `OP_DB_RPC` / `POST /rpc/<fn>`
   → `pgf_api_rpc` → `pgf_build_rpc` (`SELECT * FROM "fn"(name := $1, …)`; fn quoted,
   arg names charset-validated + emitted bare, values bound — injection-safe). Deny-by-
   default whitelist `pgf_policy_rpc_allows`: a function must be listed under `"_rpc"`
   to be reachable by anyone (even a superuser — the whitelist is the exposure boundary),
   then a superuser bypasses the per-function `roles`. Runs through `run_rows` under the
   caller's tenant context (RLS), so RPC obeys the same isolation as writes; results
   serialize via a new table-less `pgf_result_to_json` (types columns by result OID).
   Proven by byte-exact `query_builder` RPC cases (incl. bad fn/arg rejection), `policy`
   `_rpc` authz (role + superuser-still-needs-whitelist + unlisted-fn-denied), and a live
   `rpc` e2e (admin gets a typed result; editor 403; non-whitelisted `pg_sleep` 403 even
   for admin; unauth denied). 15 suites green, ASan/UBSan clean. taxi example adds an
   `_rpc` block (request_ride/accept_ride/complete_ride). Keeps the C core domain-agnostic
   (§8.5): business logic is SQL.
7. **#55 ✅ DONE (session cache).** In-memory session cache — eliminates the
   per-request `pgf_auth_verify` DB join when enabled. `src/core/session_cache.{c,h}`
   (chained hash, mutex, lazy TTL sweep, bounded); `pgf_auth_resolve` (cache→DB→put)
   is what `pgf_identity_from_token` now calls; `pgf_auth_logout` evicts. **Opt-in**
   via `PGF_SESSION_CACHE_TTL` (seconds; default 0 = always-DB, so existing behavior
   and instant out-of-band revocation are unchanged). Coherence (M-1/M-2): the cache
   is **per-process**. The instance performing a logout/reset/revoke evicts its own
   cache immediately, but a change made out-of-band — the `revoke-sessions` CLI, a
   role change, `suspend-tenant` / tenant reassignment run in a *separate* process —
   is only seen by a running server after that entry's TTL expires. So token
   revocation, role, active-state and tenant changes have a fleet-wide propagation
   latency of **up to the TTL** when the cache is enabled; keep the TTL short if you
   need prompt revocation (0 = always-DB = instant). This bound is surfaced, not
   hidden: the server logs a warning at startup when the cache is on, and the
   `revoke-sessions` / `suspend-tenant` CLIs warn that running servers may still
   honor the previous state for up to the TTL. (A cross-process LISTEN/NOTIFY
   invalidation was considered and deferred — disproportionate for an opt-in cache.) DB-free `session_cache` unit (hit/miss/refresh/
   evict/disabled/TTL-expiry) + live `session_cache_e2e` (a token whose session row
   was deleted still resolves → proof the cache is on the auth path). 17 suites green,
   ASan/UBSan clean.
7b. **#55b** Runtime provisioning API — graduate the Phase 7 lifecycle CLIs
   (`create-tenant`, `create-platform-admin`, `suspend`/`resume`) to authenticated
   REST endpoints for `platform_admin` (pooled mode). Deferred: the CLIs already cover
   bootstrap + ops, so this is a convenience, done when online tenant management is needed.

**Discipline (carry from Phase 7):** roles are opaque strings; domain behavior is SQL,
not C; the engine has no `if (business_type)` branch. A taxi backend and a store backend
differ only in their config bundle (schema + roles + policies + topics + SQL functions).

---

## 7d. What's next (post Phase 8)

Phase 8's capability surface is complete (#50–#54) and the first scaling lever (#55
session cache) is in. pgforge is a feature-complete single-binary application backend.
Remaining candidates, roughly by value — **revisit and re-rank when returning to
pgforge** (a portico TLS pass is being done first):

**Phase 9 — production hardening** (the recommended next pgforge work):
- **Rate limiting & abuse hardening ✅ DONE.** Closed the Argon2id brute-force / CPU-DoS
  hole on `/auth/login` + `/auth/register`. `src/core/rate_limit.{c,h}` — a per-key
  token bucket (mutex'd hash, lazy idle-sweep) keyed on `portico_req_client_ip(req)`
  (proxy-aware when `PGF_TRUST_PROXY=1`); over the limit → 429. Configured by
  `PGF_AUTH_RATELIMIT="N/W"` (default 10/60; 0 disables). Both auth endpoints share the
  per-IP bucket. Proven by DB-free `rate_limit` unit (burst/deny/refill/disabled/per-key)
  + live `rate_limit_e2e` (3 reach auth, rest 429); ASan clean. The test harness defaults
  `PGF_AUTH_RATELIMIT=0` so functional tests aren't throttled. **Required portico to
  expose the client IP first** (done) — mechanism (IP) in portico, policy (the limit) here.
  *Future: a global per-IP/identity request limit and WS-login throttle.*
- **Auth hardening — foundational set ✅ DONE.** The retrofit-painful pieces, done upfront
  (the core was already sound: Argon2id, opaque revocable tokens, decoy-hash anti-enumeration,
  is_active, now rate-limited + TLS):
  - **Session tokens hashed at rest** — `pgf_sessions.token` stores `sha256(token)` (new
    `pgf_token_hash`); the client keeps the raw token, the server hashes-and-looks-up. A DB
    read-leak yields useless hashes. Migration 002 clears old plaintext-token rows.
  - **Password length cap** (`MAX_PASSWORD_LEN` 128) on login/register/create — closes the
    giant-password Argon2id CPU-DoS.
  - **`pgf_auth_revoke_user_sessions(email)`** primitive (+ clears the session cache) and a
    `pgforge revoke-sessions <email>` CLI — "log out everywhere" / force-logout, the base for
    a future password-change endpoint.
  Proven by `auth_hardening` e2e (hash-at-rest via psql, revoke kills the token, long-pw 400);
  20 ctest green, ASan clean. *Email deferred (keeps deps to libpq+libsodium): password reset,
  email verification, MFA, account lockout, audit log, session listing remain incremental.*
- **Observability ✅ DONE.** `GET /metrics` in **Prometheus text exposition format** so a
  deployment is operable. `src/core/metrics.{c,h}` — a dependency-free leaf (like
  rate_limit/session_cache): lock-free atomic counters on the hot path, a mutex'd
  request-latency histogram, and live gauges *pulled* at scrape time via a provider the
  host registers (so metrics never reaches up into db/realtime). Instrumented:
  - **requests**: `pgf_http_requests_total{status=2xx|4xx|5xx}` + a
    `pgf_http_request_duration_seconds` histogram — recorded once at the `http_routes.c`
    router boundary (an inner `route()` returns the status; the wrapper times + classifies).
  - **auth**: `pgf_auth_logins_total{result=ok|fail}` (in `pgf_auth_login`, so REST **and**
    WS count), `pgf_auth_ratelimited_total` (the 429 paths), `pgf_session_cache_lookups_total{result=hit|miss}` (in `pgf_session_cache_get`, enabled-only).
  - **DB pool**: `pgf_db_pool_connections{state=in_use|total}` (new `db_connection_pool_stats`),
    `pgf_db_pool_waits_total` (blocked on a free conn), `pgf_db_acquire_failures_total`.
  - **realtime / connections**: `pgf_realtime_subscriptions` (new `pgf_realtime_count`),
    `pgf_realtime_events_total`, `pgf_active_connections` (new **`portico_active_connections`** —
    sums per-thread active conns; the on_connect/on_disconnect approach under-counts because
    portico fires on_disconnect for plain-HTTP closes that never upgraded → mechanism in portico).
  - **process**: `pgf_uptime_seconds`, `pgf_build_info{version}`.
  Route `GET /metrics` in `http_routes.c` — **unauthenticated, always on** (scrape on a
  trusted net / bind localhost / gate at the proxy; documented in `dist/pgforge.env.example`).
  Proven by a DB-free `metrics` unit (counters, gauge provider, full Prometheus render incl.
  cumulative histogram buckets) + a live `metrics_e2e` (scrape → drive traffic → re-scrape,
  asserts 2xx / latency-count / failed-login counters advanced). 22 ctest green, ASan/UBSan clean.
- **Input/DoS limits review ✅ DONE.** Audited the request path and closed the two real gaps:
  - **Slowloris** — portico never enforced its `handshake_timeout` (dead config). Added a
    1 Hz reaper in the event-loop that closes connections still in TLS-handshake / initial-read
    states past the timeout (measured from accept via `conn->connect_time`, so dribbling can't
    reset it). A connection leaves those states the instant it completes its first request, so
    established WS / idle keep-alive are never touched. Configurable via `PGF_HEADER_TIMEOUT`
    (default 15s).
  - **Oversized bodies** — the engine accepted up to portico's 16 MB buffering cap. Added a
    pgforge-layer body cap (`PGF_MAX_BODY`, default 1 MiB): an over-limit body gets 413 **before**
    the JSON parser runs (CPU/memory-amplification guard).
  Already-sound limits (audited, documented): headers ≤ 64 / 32 KiB → 431, LIST ≤ 1000 rows,
  query string 4 KiB, table/fn/id name caps → 414, password ≤ 128, cJSON nesting limit. Proven by
  a `limits` e2e (2 KB body with a 1 KB cap → 413; a held-open partial request reaped after the
  timeout). 37 ctest green, ASan/UBSan clean. *Noted for later: portico's WS pong-timeout is also
  unenforced (dead-WS-peer detection); per-IP connection cap; lowering portico's 16 MB buffering
  ceiling to a config.*

**Feature depth:**
- **Richer read API — embedding + exact count ✅ DONE.** Wired `OP_DB_QUERY` (0xD5) and
  extended `GET /api/<table>` with two opt-in query params:
  - **`embed=<related-table>[,…]`** — relationship embedding off the catalog's FK graph
    (`is_fk`/`fk_table`/`fk_column`, already introspected). **to-one** follows a forward FK
    on the base table (`products.category_id` → `categories`); **to-many** follows a reverse
    FK on the related table (`categories` ← `products`). Implemented **app-side** in
    `api.c` (`resolve_relation`+`embed_one`): one scoped `… WHERE remote_key IN (base keys)`
    per relation (not N+1), stitched in C by canonical-JSON key match. **Crucially, each
    embedded read re-runs the caller's SAME authz** — `pgf_policy_allows` + `make_scope` +
    the RLS tenant binding — so you can't leak related rows you couldn't read directly
    (proven by `query_authz`). Embed targets come only from FK edges (quoted, never client
    strings); ambiguous/unknown relations → 400.
  - **`count=exact`** — adds `"total"` (the unpaginated row count under the same
    filters+scope) alongside the page's `"count"`. New `pgf_build_count` reuses the shared
    `build_where`, so the total always matches the result set. (`"count"` already meant
    page size, so exact total is the new `"total"` field.)
  Proven by `query_builder` count cases + `query` e2e (to-one, to-many, exact + filtered
  total, FK-graph bounds, self-cleaning fixture) + `query_authz` e2e. 24 ctest green,
  ASan/UBSan clean.
  - **Boolean filter tree ✅ DONE.** The flat-AND `where` extends to a tree: `and`/`or`/`not`
    nesting + a `between` operator (recursive `build_node` in query_builder; logical keys
    reserved, columns still catalog-validated, values bound — injection-safe; the flat form is
    byte-for-byte unchanged). Reachable over WS (JSON `where`) and REST via `?where=<url-encoded
    JSON>`, which merges (AND) with any flat `col=op.val` filters. Byte-exact `query_builder`
    cases + a `query_filter` e2e (and/or/not/between, nested, flat+tree) against live Postgres.
  - **Nested/dotted embed ✅ DONE.** `embed=order_items.product` embeds the first relation, then
    recurses into the embedded rows for the rest (`embed_path`; gathers them by cJSON reference so
    the deeper embed mutates in place — no copy, ASan-clean). **Authz + row-scope re-run at every
    level** (each goes through `embed_one`). Depth-capped (4); works both directions
    (to-many→to-one, to-one→to-many). `query_nested` e2e on the demo's circular relation.
  - **Group-by aggregates ✅ DONE.** `?group=category_id&aggregate=count,sum:price,avg:price` →
    `SELECT <group cols>, count(*)/sum/avg/min/max(...) FROM t WHERE <filters+scope> GROUP BY …
    ORDER BY <group cols>`. A different result shape, so `pgf_api_list` short-circuits to it
    (OID-typed via the table-less `run_rows`). **Runs under the same filters + row scope**, so a
    total never includes rows the caller can't see. Functions whitelisted, columns catalog-
    validated + quoted; `group` alone = distinct values, `aggregate` alone = totals. Byte-exact
    `query_builder` cases + a `query_agg` e2e (count/sum/avg/min/max, totals, bad fn). *Still
    deferred: JSON-field access in filters.* **Richer-read depth (#52–#54) complete.**
- **Load/perf harness ✅ DONE.** `bench/` — a stdlib-only (no wrk/hey) closed-loop generator
  (`loadtest.py`, multi-process so the GIL doesn't cap it; reports req/s + p50/p90/p99) and a
  runner (`bench.sh`) that boots pgforge with demo data, rate-limiter off, and benchmarks
  health / authed-list / authed-get / login plus a session-cache on/off comparison. A manual
  tool, not in ctest. Baseline (32-core loopback): health ~113k req/s (0.17ms) — transport is
  not the bottleneck; authed read ~8.5k (2.3ms) DB-bound → **~18k (1.1ms) with the session
  cache (~2.1×)**; login ~72 req/s (107ms — Argon2id, which is exactly why login is
  rate-limited + lockable). See `bench/README.md`. *Next perf steps: measure under real network,
  with the connection pool sized up, and profile the authed-read path.*
- **Auth completeness** — federated login (Google/Apple), 2FA, richer user model,
  password reset / email verification, account lockout. **Designed in §7e** (model +
  migration plan; nothing built yet). The retrofit-painful piece is the credential split
  (`pgf_identities`); 2FA (TOTP) is no-new-deps C; full interactive OAuth is the part that
  fights single-binary (do ID-token verification, not the redirect dance).
- **CORS ✅ DONE.** Browser SPAs on another origin can call the API. `core/cors.{c,h}`
  holds the origin policy (`PGF_CORS_ORIGINS=<comma list>|*`, off when unset; optional
  `PGF_CORS_CREDENTIALS`); the HTTP router echoes `Access-Control-Allow-Origin` (+ `Vary:
  Origin`) on allowed responses and answers `OPTIONS` preflights with 204 + allow-methods/
  headers/max-age. Opt-in and locked down by default. DB-free `cors` unit (origin matching,
  wildcard, credentials) + a `cors_e2e` (curl: echoed/denied/none + preflight). *(ASan caught
  a stack-use-after-scope — the allowed-origin pointer aliased a block-scoped buffer; fixed by
  returning stable pointers + hoisting the buffer to function scope.)* 34 ctest green, ASan clean.
- **Keyset/cursor pagination ✅ DONE.** Constant-time deep pagination + stable-under-writes,
  as an opt-in alongside the default LIMIT/OFFSET. `GET /api/<table>?...&cursor=` (empty =
  first page) returns rows + an opaque `next_cursor`; pass it back for the next page. The
  effective order is the request's `order` columns + the PK appended as a tiebreaker (total
  order); the cursor is base64url(JSON of the last row's key values). `query_builder` builds
  the keyset path (`pgf_resolve_sortkeys`; an expanded lexicographic `col OP $n` predicate so
  the bind type is inferred per column — works for uuid/text/numeric; no OFFSET); `api.c`
  decodes/encodes the cursor (`core/base64url.{c,h}`) and emits `next_cursor` only on a full
  page. Filters + row-scope still apply; same injection-safety. Constraint: all `order`
  columns must share one direction (mixed → 400); no random page jump (use OFFSET for that).
  Byte-exact `query_builder` keyset cases + `base64url` unit + a `query_keyset` e2e (7 rows,
  limit 3 → pages tile exactly, no gaps/dups, last page no cursor, bad cursor → 400). 36
  ctest green, ASan/UBSan clean.

**Scale-out** (deferred until multi-instance OR out-of-band DB writers are real):
- **#53b** — LISTEN/NOTIFY realtime source: multi-instance fan-out **and** surfacing
  writes made outside pgforge (so it also *completes* single-instance realtime). Carries
  a `pg_notify`-per-write tax on enabled tables; source becomes a config knob.
- **#55b** — runtime provisioning REST API: convenience over the working Phase 7 CLIs;
  lowest value of everything here — do only on a concrete self-service-tenant need.

**Transport (portico):**
- **Optional in-process TLS** — make portico deployable *without* nginx (one self-contained
  binary). Link OpenSSL (likely already in-process via libpq → no new dep), terminate TLS
  in the epoll loop, load cert/key from disk; keep plaintext mode so proxy deployments
  (multi-instance LB, WAF, HTTP/2) still work. The biggest, most security-critical portico
  change — touches the hardened non-blocking read/write path. *(Being done next.)* A
  lighter alternative if "no nginx" just means "easier TLS": front with **Caddy**
  (auto-HTTPS via ACME, ~zero config) — a docs change, not code.

---

## 7e. Auth completeness — design (not yet built)

The current model assumes **one credential (password) per user, inline in `pgf_users`**.
Federated login and 2FA both break that assumption, so the model is designed here before
any code. The unifying principle: **every login method converges on the same opaque
`pgf_sessions` token** — so authz, RLS, the session cache, and "revoke everywhere" are
unchanged regardless of *how* a user authenticated. The login method is pluggable; the
session is uniform. `pgf_user_t` (id/email/role/tenant) — the request context downstream
policy sees — also does not change.

`pgf_users` is currently doing three jobs that should separate: the **account** (id, role,
is_active, tenant), the **credential store** (password_hash inline), and the **profile**
(implicitly, email). Split them:

```
pgf_users         ── the account/person (id, role, is_active, tenant_id, primary email)
  ├─ pgf_identities  ── 1-to-many: one row per login method
  ├─ pgf_mfa         ── 1-to-many: second factors (TOTP, recovery codes)
  └─ user_profiles   ── app data (name, phone, avatar, …) — an ordinary app table, NOT pgf_
```

**1. Profile fields = app data (no engine change).** The many "real user" fields are not
auth data. Put them in a normal CRUD table keyed by `user_id`, policy-scoped with
`owner_column = user_id` (each user reads/writes only their own), exposed at
`/api/user_profiles`, joinable via the richer-read `embed`. Keep `pgf_users` lean — only the
columns the auth path needs. This is the "configure, don't fork" answer; it already works
today.

**2. Credential split — `pgf_identities` ✅ DONE (migration 003).** One account, many login
methods; the password credential moved out of `pgf_users` into a `provider='password'`
identity, and `pgf_users.password_hash` was dropped. Login now resolves *through* the
identity (`WHERE provider='password' AND provider_uid=email`) — the same shape a federated
login will use (`provider='google'`, uid = the `sub`). `register`/`create_user`/`seed_user`
write account + identity (+ session) **atomically** in a transaction; the backfill+drop
migration is atomic too (the runner wraps each file in `BEGIN…COMMIT`). The tenant **export**
now also dumps `pgf_identities` (scoped via `user_id`) so a graduated deployment keeps working
credentials. Proven by an `identity` e2e (column gone, seeded + admin-created users get a
working `password` identity, login/401 intact), updated `migrate` counts, and a
credential-continuity assertion in `graduate`. 25 ctest green, ASan/UBSan clean. The original
design:
```sql
CREATE TABLE pgf_identities (
  id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id uuid NOT NULL REFERENCES pgf_users(id) ON DELETE CASCADE,
  provider text NOT NULL,        -- 'password' | 'google' | 'apple' | 'github'
  provider_uid text NOT NULL,    -- email for password; the OIDC 'sub' claim for federated
  secret text,                   -- argon2 hash for password; NULL for OAuth
  UNIQUE (provider, provider_uid)
);
```
One person, many methods (password AND Google AND Apple → the same `pgf_users` row). The
migration moves `pgf_users.password_hash` into a `provider='password'` row. Changes
credential storage shape, so painful to retrofit once there's data — same reasoning as the
token-hashing pass; sequence it first. Login by any identity resolves to `user_id` → the
existing session machinery is unchanged.

**3. 2FA (TOTP) ✅ DONE (migration 004).** Standard RFC 6238 (6-digit / 30s / HMAC-SHA1, from
OpenSSL — already in-process via portico TLS / libpq, no new dep) in `totp.{c,h}` (base32 +
HOTP truncation; proven against the official RFC test vectors). The DB flow is in `mfa.{c,h}`:
`pgf_mfa` (one enrollment, `confirmed_at` gates login) + `pgf_mfa_challenges` (single-use,
hashed, TTL'd, attempt-capped). **Opt-in + per-user:** `PGF_MFA=optional` (default `off` =
fully inert) AND the second step only fires for a user with a *confirmed* enrollment — so an
untouched deployment is unchanged (the basic-app vs. payments-app knob). Login now branches: on
a confirmed enrollment it returns `{status:"mfa_required", challenge}` instead of a session;
`POST /auth/mfa/verify {challenge, code}` (public, rate-limited) completes it. Enroll/confirm/
disable act on the authenticated caller (disable needs a current code). Session minting was
**factored out into `pgf_auth_issue_session`** — shared by the password path, MFA verify, and
(next) OAuth — so all methods converge on the same opaque token (authz/RLS/cache unchanged).
Admin lockout recovery: `pgforge mfa-reset <email>`. Proven by a `totp` unit (RFC vectors) + an
`mfa` e2e (full enroll→confirm→two-step-login→disable, codes from an independent Python TOTP =
cross-validation; wrong-code 401; single-use challenge). 27 ctest green, ASan/UBSan clean.
*Deferred: self-service recovery codes (admin `mfa-reset` covers lockout for now); `required`
mode (force enrollment) — needs a limited "enrollment grant" flow.* The original design:

A second factor on an existing identity, not a new identity:
```sql
CREATE TABLE pgf_mfa (
  user_id uuid REFERENCES pgf_users(id) ON DELETE CASCADE,
  type text,               -- 'totp'
  secret text,             -- base32 seed (encrypt at rest)
  confirmed_at timestamptz -- NULL until the user proves one code
);
```
Two-step login: password verifies → if confirmed MFA exists, return a short-lived
**MFA-pending challenge token** (NOT a full session); client submits the 6-digit code →
verify → issue the real session. TOTP (RFC 6238) is HMAC over a time counter; **libsodium
already provides HMAC** → no new lib. Add base32 (for the `otpauth://` QR URI) and hashed
single-use recovery codes (lockout recovery). Highest value / lowest cost. (WebAuthn/passkeys
are the modern alternative but much heavier — CBOR, attestation — deferred.)

**4. Google / Apple (OIDC) — ✅ DONE (Option B).** `POST /auth/oauth {provider, id_token}`:
`oauth.{c,h}` verifies the token's **RS256 signature** (OpenSSL 3 `EVP_PKEY_fromdata` builds the
RSA pubkey from the JWKS `n`/`e`; `EVP_DigestVerify`) against the provider's **JWKS, fetched +
cached via libcurl** (the chosen dependency — small, ubiquitous, dynamically linked like libpq;
self-fetch + refetch-on-unknown-`kid` so key **rotation is a non-event**), then checks
`iss`/`aud`/`exp`. Providers are generic/config-driven: `PGF_OAUTH_PROVIDERS=google,apple,…` with
per-provider `_CLIENT_ID`/`_ISSUER`/`_JWKS` (google + apple ship built-in issuer+JWKS). The DB half
is `pgf_auth_oauth_login` in auth.c: existing identity → login; else a provider-**verified** email
matching an account → link; else auto-provision with the default signup role (so it obeys the
self-registration policy) — all via the shared `pgf_auth_issue_session`, same opaque token. We do
**not** run the interactive redirect/code dance. Proven by an `oauth` e2e with a **mock OIDC
provider** (throwaway RSA key, local JWKS server, `openssl`-signed tokens — fully offline):
auto-provision / replay / link-by-email succeed; tampered-signature / wrong-aud / expired /
unknown-kid / unknown-provider all 401. 28 ctest green, ASan/UBSan clean.

The original design — it needs an outbound HTTPS client and JWT/JWKS verification; three options:
- **(A) pgforge runs the full OAuth dance** — link libcurl, do the redirect + code exchange,
  fetch JWKS, generate Apple's signed client-secret, verify RS256. Biggest new surface, a
  real new dependency; fights the design.
- **(B) pgforge verifies a pre-obtained ID token** — *recommended.* The SPA/mobile app gets
  the ID token from Google/Apple (their SDKs are excellent), then calls
  `POST /auth/oauth { provider, id_token }`. pgforge verifies the token's RS256 signature
  against the provider's **cached JWKS**, checks `iss`/`aud`/`exp`, extracts `sub`+`email`,
  finds-or-links the `pgf_identities` row, and issues a session. **OpenSSL is already
  in-process** (portico TLS / libpq), so RSA verification needs no new dep — and it skips the
  redirect, the code exchange, and storing client secrets. A Google/Apple login also yields a
  **pre-verified email** (sidesteps the SMTP-for-verification problem). The one outbound call
  is the JWKS GET, cacheable for hours.
- **(C) front with a dedicated IdP** (Keycloak/Auth0/Supabase Auth/Ory) issuing tokens
  pgforge validates — consistent with §7c.5 ("heavy/external-integrating logic lives in a
  companion service; pgforge is the data plane"). Least code here, one more thing to run.

**5. Email transport — ✅ DONE (mailer).** The SMTP "external dependency" turned out to be a
non-issue: libcurl (already linked for OIDC) speaks SMTP, so **no new library**. `mailer.{c,h}`
builds an RFC 5322 message and sends it via libcurl (`smtp://`+STARTTLS or `smtps://`, optional
AUTH, header-injection guard). Opt-in (`PGF_SMTP_URL` + `PGF_MAIL_FROM`; `PGF_SMTP_TLS=require`
by default); inert otherwise. `pgforge send-test-mail <to>` verifies an operator's config. Proven
by a DB-free `mailer` e2e against a mock SMTP sink (asserts a well-formed message). The only real
cost is *operational* — the deployment supplies a relay/credentials + deliverability (SPF/DKIM).
**Now password reset + email verification just build on `pgf_mail_send`** (token tables + a couple
of endpoints) — the remaining auth work. Account **lockout** needs no email (track failed attempts
per identity, lock after N — pairs with the rate limiter). OIDC logins arrive email-verified.

**Status — auth track COMPLETE:** ✅ (1) `pgf_identities` split, ✅ (2) TOTP 2FA, ✅ (3)
`/auth/oauth` (Option B), ✅ email transport (mailer), ✅ password reset, ✅ email verification,
✅ account lockout, ✅ **TOTP recovery codes**. **Next: the breadth tier** — CORS (unblocks
browser SPA frontends), keyset/cursor pagination, input/DoS-limits review, richer-read depth
(nested embed / aggregates / boolean filters). Still optional/anytime: the `user_profiles`
pattern (zero engine change) and TOTP `required` mode (force enrollment).

**TOTP recovery codes ✅ DONE (migration 008).** `pgf_mfa_recovery` (sha256(code), single-use
`used_at`). Confirming TOTP enrollment now issues **10 one-time codes** (shown once in the confirm
response); `pgf_mfa_verify_login` accepts a recovery code in place of a TOTP code and consumes it
(normalize → sha256 → atomic `UPDATE … used_at IS NULL RETURNING`). `POST /auth/mfa/recovery-codes`
(Bearer + a current TOTP code) regenerates the set (invalidating the old one); `disable` deletes them.
This is self-service lost-authenticator recovery, so admin `mfa-reset` is the fallback, not the norm.
Proven by the extended `mfa` e2e (confirm returns 10 codes; a code completes login; single-use;
regenerate kills the old set). 32 ctest green, ASan/UBSan clean.

**Account lockout ✅ DONE (migration 007).** Opt-in per-account brute-force defense complementing the
per-IP rate limiter. `PGF_AUTH_LOCKOUT="N/W"` (default `0`/off): after N failed password logins within
W seconds, lock the account for W seconds — **a correct password is refused while locked** (the point
of lockout). State on `pgf_users` (`failed_login_count`, `last_failed_login_at`, `locked_until`), all
read in the existing login query; `pgf_auth_login` checks the lock, increments the streak on failure
(resetting it if the last failure was outside the window), and clears it on success. The lock
auto-expires (time-based, bounds the lock-the-victim DoS — why it's opt-in). Admin recovery: `pgforge
unlock <email>`. Proven by a `lockout` e2e (3 fails lock; locked-even-with-correct-pw 429; auto-unlock
after the window; post-reset single failure doesn't lock; admin unlock). 32 ctest green, ASan/UBSan clean.

**Email verification ✅ DONE (migration 006).** `pgf_users.email_verified_at` + `pgf_email_verifications`
(sha256(token), 24h TTL, single-use). A verification email is sent on **register** (best-effort via the
mailer); `POST /auth/verify-email {token}` redeems it (atomic claim → set `email_verified_at`); `POST
/auth/verify-email/resend` (Bearer) re-sends for the caller (idempotent). The flag is surfaced as
`user.email_verified` in login/register/oauth/verify-session responses (new `pgf_user_t.email_verified`,
filled by the login + verify queries). **OIDC logins arrive verified** — `pgf_auth_oauth_login` sets
`email_verified_at` when the provider vouched for the email and it matches the account. Not *enforced*
yet (login works regardless; blocking unverified users is a future policy knob). Proven by an
`email_verification` e2e (register → token captured from mock SMTP → flag false → redeem → flag true;
single-use; bad token 400; resend idempotent). 31 ctest green, ASan/UBSan clean.

**Password reset ✅ DONE (migration 005).** `pgf_password_resets` (sha256(token), TTL, single-use
`used_at`). `POST /auth/password/forgot {email}` → `pgf_auth_create_password_reset` mints a token for
the account's `password` identity and the API emails it via `pgf_mail_send` (link uses `PGF_APP_URL`,
or the raw token) — **always 200** (anti-enumeration); rate-limited. `POST /auth/password/reset
{token,password}` → `pgf_auth_perform_password_reset` atomically claims the token (`UPDATE … WHERE
used_at IS NULL AND expires_at>now() RETURNING`), updates the identity secret, and **revokes the
user's sessions**. Proven by a `password_reset` e2e (token emailed → captured from a mock SMTP sink →
redeemed; new pw works, old pw + old session 401; single-use; weak-pw 400; anti-enumeration). 30 ctest
green, ASan/UBSan clean.

---

## 8. Open decisions

1. ~~Transport~~ — **DECIDED: REST front door + WS realtime** (see §3a).
2. **Policy storage** — JSON config file vs. a `policies` table in Postgres (live-editable
   from the admin UI). Start with file; migrate to table later.
3. ~~Multi-tenancy model~~ — **DECIDED: shared DB + `tenant_id` column, as the
   degenerate-superset of single-tenant** (see §7b). Supports both Model A (off) and
   Model B (on) from one engine; big tenants can graduate to their own DB.
4. ~~RLS~~ — **DECIDED: app-level scope rules are the primary guard; generate Postgres
   RLS + `SET LOCAL` as defense-in-depth in pooled mode** (see §7b, task #46).
5. ~~Domain logic location~~ — **DECIDED: SQL functions exposed as RPC** (engine stays
   domain-agnostic — "configure, don't fork"); heavy or external-integrating logic
   (payments, dispatch) lives in **separate services** that use pgforge as their data
   plane (see §7c.5, Phase 8).

---

## 9. Risks

- **Security surface**: generic CRUD + C string handling. Mitigate with strict
  parametrized queries only, deny-by-default policies, fuzzing the JSON/protocol
  parsers, and ASAN in CI.
- **Self-containment**: the source depends on sibling `lib/*` static libs — vendor
  them into the repo so the project builds standalone.
- **Type coverage**: Postgres has many types; start with the common set (int, numeric,
  text, bool, uuid, timestamptz, json) and expand.
