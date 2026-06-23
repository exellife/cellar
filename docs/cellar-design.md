# cellar — design

cellar is a **per-app backend engine**: point it at a directory of apps and it serves
each one as a REST + realtime API generated from that app's schema, with per-app
business logic. Same split as the family — **portico is the transport (mechanism),
cellar is the engine (policy)**. Where pgforge is *one app / many tenants* over a
shared Postgres (isolation by a `tenant_id` column), cellar is *many apps / each its
own database* — **one SQLite file per app** (isolation by the file). That makes per-app
schemas and per-app logic first-class: different apps can have entirely different
tables, and ship their own behavior, without touching the engine.

Status: design. Forked from pgforge; the SQLite pivot and everything below is to build.
This doc is the contract — code follows it; when they disagree, fix one on purpose.

---

## 1. The one principle (inherited): mechanism vs policy

- **portico = MECHANISM** — bytes, sockets, framing, TLS, the event loop, backpressure,
  and (new) the **tunnel** that exposes a home origin publicly. Knows nothing about
  apps, SQL, auth, or hooks.
- **cellar = POLICY** — routing an app, its schema/catalog, generic CRUD, authz,
  realtime, and the **per-app extension layer** (hooks). It *uses* portico for transport.

Decision rule for any capability: *"could any HTTP/WS app need this?"* → portico.
*"is it about an app's data, identity, or behavior?"* → cellar.

## 2. Locked decisions (v1)

| Area | Decision | Rationale |
|---|---|---|
| Data model | **one SQLite file per app**; isolation is the file | per-app schema for free; trivial provision/delete/move; no shared-catalog limits |
| Catalog | **per-app**, introspected from `sqlite_master` + `PRAGMA` | replaces pgforge's single global `g_active` |
| Extensibility | **3 layers**: SQLite-native → **Lua hooks** (a fixed contract) → compiled-C escape hatch | push logic down; script the rest; native only on the hot path |
| Hook runtime (v1) | **LuaJIT** (embedded, FFI on) | C-API compatible embed; **FFI** calls cellar's C hook API with no binding glue + near-zero overhead; fast interpreter/JIT |
| Trust model (v1) | **first-party apps** (you write them) | relaxes sandboxing — LuaJIT FFI is unsandboxable, which is fine (a convenience) here; WASM is the third-party upgrade path, not v1 |
| Primary deployment | **home origin behind portico-tunnel** | data lives on your box; relay only moves ciphertext → data sovereignty |
| App unit | a **bundle**: `data.db` + `hooks.lua` + `policies.json` | schema, data, *and behavior* are portable as one directory |
| Concurrency | SQLite calls + hooks run on **DB worker threads**, never the event loop | SQLite is blocking + single-writer; the loop must never stall |

## 3. Deployment topology

cellar is **deployment-agnostic** — it's just an origin that is a pile of `.db` bundles.
Two shapes:

- **VM-direct** — cellar is the public origin on a cloud box. Simple.
- **Home-behind-tunnel (primary)** — cellar runs on the home box (e.g. `srvlab`);
  **portico-tunnel** exposes it publicly: the relay SNI-routes `app.example.com :443`
  to the home origin, **never decrypting** (passthrough), and the cert is issued/renewed
  through the tunnel (ACME-through-tunnel). The agent dials out, so a dynamic/NAT home
  IP is irrelevant.

The home shape is the point: **app data (the SQLite files) physically lives at home and
never leaves** — the cloud relay sees only ciphertext. cellar + portico-tunnel is a
self-hosted backend with a public face the host can't read.

```
  app1.example.com ┐                      ┌ relay (cloud VM) ┐   one mTLS tunnel   ┌ agent (home) ┐
  app2.example.com ┼─ :443 SNI ──────────►│  passthrough,    │◄══════════════════►│  → cellar    │
  app3.example.com ┘                      │  never decrypts   │                    │   (origin)   │
                                          └ :80 → ACME ───────┘                    └─ apps/*.db ──┘
```

### 3.1 Multi-tenant subdomains & TLS

Routing is by the **full Host header**, normalized and used verbatim as a bundle
directory name — there is no special "subdomain" parsing and no wildcard *matching* in
the engine (`cel_apps_resolve`):

```
app1.example.com → $CEL_APPS_DIR/app1.example.com/data.db
app2.example.com → $CEL_APPS_DIR/app2.example.com/data.db
```

- **Normalization** (`cel_apps_norm_host`): lowercase, strip `:port`, allow only
  `[a-z0-9.-]`, reject a leading dot/dash and any `..`. The host doubles as a path, so
  it's validated tightly — no traversal.
- **No auto-create.** Resolve `stat()`s `$CEL_APPS_DIR/<host>/`; a missing directory →
  no app (request 404s). Each FQDN is provisioned explicitly (`cellar provision <host>
  …`) → default-deny by construction. There is **no `*.example.com` wildcard match**.
- **Optional registry gate** (`CEL_CONTROL_DB`, §11): when set, only registered+active
  hosts route; `suspend`/`resume` take effect live (read per request).
- Apps open lazily and are **pinned** in a cache up to `CEL_APPS_MAX` (1024) distinct hosts.

Full isolation per host: separate `data.db`, catalog, `hooks.lua`, `policies.json`,
`public/`, and **users**. Sharing a parent domain is a DNS fact only — cellar keys on the
whole FQDN. This is the **multi-tenant-by-subdomain** model: one tenant = one bundle =
one FQDN, with zero crosstalk.

**TLS is the part to plan, not routing.** cellar loads a **single** static cert
(`CEL_TLS_CERT` / `CEL_TLS_KEY`) — no SNI-based multi-cert selection, no built-in ACME.
To serve many subdomains over HTTPS, give it one cert that covers them all:

- **Wildcard `*.example.com` (recommended for dynamic tenancy).** One cert covers all
  current + future single-level subdomains, so adding a tenant is just `provision` +
  DNS — no cert work. Pair it with a **wildcard DNS** record (`*.example.com → IP`) and
  provisioning becomes a purely cellar-side action.
  - Requires **DNS-01** ACME (a wildcard can't use HTTP-01) → needs DNS-provider API access.
  - Covers **one label only**: not the apex `example.com` (add it as a SAN:
    `-d '*.example.com' -d example.com`) and not `a.b.example.com` (needs `*.b.example.com`).
- **Multi-SAN cert** — lists each host; works with HTTP-01 but must be reissued on every
  new tenant → only for a small/fixed set.
- **TLS-at-the-edge** — terminate upstream and run cellar as a plaintext origin. The
  portico relay's SNI passthrough already does this; a proxy like Caddy with on-demand
  TLS is the other form. This is the path for **custom tenant domains**
  (`bookings.theirhotel.com`), which a wildcard cannot cover.

**Engine gaps this surfaces (backlog).** cellar reads its cert once at boot, so a
wildcard **renewal currently needs a restart** — a `SIGHUP`/file-watch cert
**hot-reload** would make renewals zero-downtime (mirroring the `hooks.lua` reload in
§9). And there is no built-in ACME — issuance/renewal is external (e.g. `certbot
--dns-<provider>`). Until those land: wildcard renewal = re-point the PEMs + restart.

## 4. The app as a bundle

```
apps/
  myapp/
    data.db        # schema + data + triggers/CHECK/generated cols (Layer-1 logic)
    hooks.lua      # Layer-2 business logic (or a `_hooks` table inside data.db)
    policies.json  # declarative authz (roles, ownership/scope rules)
    public/        # the app's front-end: HTML/CSS/JS/images/fonts (served static)
```
Provision = drop a directory; delete = `rm -rf`; export/backup/move = copy. Routing maps
a Host/subdomain → a bundle. Because the bundle is self-contained, an app's schema,
data, **behavior, and front-end** travel together.

`public/` is served by `cel_http_router`: it resolves Host → app (as it already does
for the DB), then tries `portico_res_static` against that app's `public/` first (with an
`index.html` SPA fallback so client-side routes like `/admin` or `/profile/:id` resolve),
and falls through to `/auth` · `/api` · `/rpc` · the realtime WS for everything else.
(`cellar provision` scaffolds `public/index.html`; the router wiring is the next step.)

## 5. Per-app isolation & catalog

- Each request resolves to **one app → one `sqlite3*` handle** for its `data.db`. A hook
  or query for app A can only ever reach app A's file. The OS file boundary is the
  isolation — no `tenant_id`, no RLS, no `SET LOCAL`.
- **Catalog is per-app**, built by introspection: `sqlite_master` + `PRAGMA
  table_info / foreign_key_list / index_list`. Cached per app; invalidated on schema
  change. (This is the `g_active` → per-app refactor pgforge couldn't avoid.)
- **Generic CRUD** dispatches on the table name against that app's catalog;
  identifiers are quoted from the catalog, values are bound (`?` placeholders) →
  injection-safe by construction. `RETURNING` is available (SQLite ≥ 3.35); type
  *affinity* (incl. `STRICT` tables) governs JSON serialization.

## 6. Concurrency contract

The hard rule that shapes everything: **SQLite is synchronous and single-writer per
file; portico is a non-blocking event loop.** Therefore:

- Every SQLite call **and every hook** runs on a **DB worker thread**, never the event
  loop (reuse pgforge's `opcode_dispatcher` pool).
- Each worker holds a small **per-app `sqlite3*` handle cache** (LRU; WAL mode → many
  readers + one writer) **paired 1:1 with a per-app `lua_State` cache** (§8).
- **Per-app write serialization**: a per-app work queue so one write-hot app waits on
  *its own* file lock without starving others.
- `lua_State` is not thread-safe → exactly one per (worker thread × app); no locking.

## 7. Three-layer extensibility

Cheapest first; reach up only when the layer below can't express it.

**Layer 1 — push logic into SQLite (free; travels in the `.db`).** Triggers
(BEFORE/AFTER INSERT/UPDATE/DELETE), `CHECK` + `STRICT`, generated columns, views,
SQL functions/queries as RPC. cellar's CRUD respects all of it automatically. A large
fraction of "business logic" lives here, declaratively, inside the file.

**Layer 2 — Lua hooks (the contract, §9), on LuaJIT.** For logic SQL can't express:
cross-cutting validation, side-effects, custom endpoints, custom authz. First-party →
trusted → we expose *useful* APIs rather than a jail. **LuaJIT** is the runtime: its
**FFI** lets hooks call cellar's C hook API (db query/exec, request context, logging)
directly — no per-API `lua_push*/lua_to*` binding glue and near-zero call overhead —
and its interpreter/JIT keeps compute-heavy hooks cheap.

**Layer 3 — compiled-C handlers (escape hatch).** For first-party hot-path logic that
must be native. Registered by name; no per-request scripting cost. Rare by design.

*(WASM is the future Layer-2 runtime for third-party/untrusted apps — same contract,
sandboxed engine. Note LuaJIT's FFI is intentionally unsandboxable, so the third-party
path is WASM, **not** a locked-down LuaJIT — consistent with §2's trust decision.)*

## 8. The hook contract

Language-agnostic signatures (the contract matters more than the runtime). A bundle
implements any subset; absent hooks are no-ops:

```
authorize(op, table, row, who)   -> allow | deny      # beyond built-in deny-by-default
before(op, table, input, who)    -> input | reject    # validate / default / transform
after(op, table, row, who)       -> void              # side effects: notify, enqueue, fan-out
rpc(name, args, who)             -> result            # custom endpoints beyond CRUD
on_realtime(change, subscriber)  -> include? filter   # who sees which change events
```
`op` ∈ {list,get,create,update,delete}. `who` is the resolved identity (role, user_id,
…). A `before` that rejects aborts the op with an error; `after` runs inside or just
after the txn (TBD per op). Hooks for an app receive a handle to **that app's DB only**.

## 9. Lua VM model (LuaJIT)

- **Runtime: LuaJIT** (Lua 5.1 C-API compatible). Build GC64 mode on so 64-bit hosts
  aren't capped at the old ~1–2 GB VM limit (irrelevant for hook-sized working sets, but
  free insurance). Targets x86_64 (srvlab/VM) and ARM64 — both supported.
- **One `lua_State` per (worker thread × app)**, created on first use by loading the
  app's `hooks.lua`, cached alongside the `sqlite3*` handle. Reused across requests →
  no per-request VM spin-up. (LuaJIT states are not thread-safe to share — same rule.)
- **Exposed to hooks via FFI (first-party, so generous):** the app's DB (parameterized
  query/exec), the request context (`who`, `input`, headers), structured logging, and a
  guarded outbound client (HTTP/queue) for side-effects. FFI means these are direct C
  calls, not hand-written bindings. Tighten/replace this surface when third-party apps
  arrive (→ WASM).
- **Hot-reload:** editing `hooks.lua` drops the cached `lua_State` (next request reloads)
  — change an app's behavior with no cellar restart, mirroring portico's config reload.
- Runs on the DB worker thread (blocking is fine there); never on the event loop.

## 10. Request lifecycle

```
TLS terminated at the origin → cellar:
  1. route      Host/subdomain → app bundle
  2. identity   resolve `who` (per-app users; control-plane for platform admin)
  3. authorize  built-in deny-by-default + optional Lua authorize()
  4. before     Lua: validate / default / transform / reject
  5. query      generic, injection-safe, in a txn on the DB worker thread
  6. (db)       SQLite triggers + CHECK + generated cols fire   ← Layer 1
  7. after      Lua: side-effects
  8. realtime   publish change (+ optional on_realtime filter)
  9. serialize  PGresult-equivalent → typed JSON
```

## 11. Identity & control-plane

- **Per-app users** live in each app's `data.db` (auth tables shipped by a base
  migration) — an app is self-contained.
- A small **control-plane DB** holds what isn't per-app: the **app registry** (name →
  bundle path, routing host), platform admins, and global ops. Provision/suspend/delete
  an app = a control-plane op + a filesystem op.
- Reuse pgforge's auth crypto (Argon2id, opaque tokens, TOTP) per app.

## 12. What ports from pgforge vs. what's rewritten

**Reuse:** portico transport, `opcode_dispatcher` (worker pool), `logger`, the
generic-opcode CRUD *pattern*, the policy-engine *concept*, auth crypto, the
embed/migrate tooling.

**Rewrite (the DB layer):**
- `db_connection.c` → per-app SQLite handle cache + per-app write serialization.
- `schema_catalog.c` → PRAGMA introspection, **per-app** (drop global `g_active`).
- `query_builder.c` → SQLite dialect (`?` binds, `RETURNING`, affinity).
- **Delete** the tenant machinery (`tenant_id`, `set_config`, `SET LOCAL`, RLS).
- App routing + the control-plane DB.

**New:** the Lua hook layer (contract + per-app `lua_State` + exposed APIs), and the
bundle tooling (provision/export/hot-reload).

## 13. Build sequence

1. **Rebrand** `pgforge → cellar` (still Postgres) — compiles under its own name.
2. **SQLite per-app core** — handle cache + write-serialization, PRAGMA per-app catalog,
   dialect, drop tenant/RLS, routing + control-plane. (The bulk.)
3. **Lua hook layer** — link **LuaJIT** (GC64); the contract; per-app `lua_State` cache;
   FFI-exposed C API; hot-reload.
4. **Realtime + bundle tooling** — change events with the `on_realtime` filter;
   provision/export; backup guidance (Litestream/rsync of bundle dirs).
5. **Deploy** behind portico-tunnel on the home box (origin), per §3.

## 14. Open decisions

- **Hooks: file vs in-db.** `hooks.lua` on disk (easy to edit/diff) vs a `_hooks` table
  inside `data.db` (fully self-contained bundle, travels in one file). Lean: file for
  dev ergonomics; allow in-db for distribution.
- **`after` transactionality** — run inside the write txn (atomic, but side-effects can't
  do external I/O safely) vs just after commit (side-effects safe, but not atomic).
  Likely: `before` in-txn, `after` post-commit.
- **Per-app routing** — DECIDED: **subdomain** (full Host → bundle dir; see §3.1). Path
  prefix (`/app/…`, fewer tunnel forwards) was the alternative but loses clean per-app
  TLS/cert scoping and the one-FQDN-one-bundle isolation. Subdomain + wildcard cert/DNS.
- **Backups** — the durability story is now the host's (data is at home): Litestream to
  cloud, or scheduled rsync of `apps/`.
- **Offline-first device sync** — clients (esp. Flutter) that hold a local SQLite mirror
  and sync across devices through cellar. Designed separately in
  [`cellar-sync-design.md`](cellar-sync-design.md) (server-authoritative delta sync with
  per-app `rev` cursor + tombstones + a `resolve` hook; CRDT/cr-sqlite as the later
  upgrade). Not built; layers on the existing authz/hook/realtime primitives.

## 15. Non-goals (v1)

- **Sandboxed third-party apps** — first-party only; WASM later (same contract).
- **Cross-app queries/joins** — the file boundary *is* the isolation; apps don't share data.
- **Heavy write concurrency in one app** — SQLite is single-writer per file; fine for
  many modest apps, a ceiling for a write-hot one.

---

## Summary

cellar is pgforge inverted onto **file-per-app SQLite**: per-app schema and per-app
behavior become first-class. Generic CRUD covers the declarative 90%; the rest is a
**three-layer extension model** — SQLite-native, then **Lua hooks over a fixed
contract**, then compiled-C for the hot path — with WASM reserved for the day apps stop
being first-party. It runs as a **home origin behind portico-tunnel**, so the data lives
on your box and the public relay only ever moves ciphertext. An app is a portable
**bundle** (`data.db` + `hooks.lua` + `policies.json`): drop it in, it's live; copy it,
it's backed up; edit its hooks, it hot-reloads.
