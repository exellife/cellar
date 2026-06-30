# Authorization in cellar — `policies.json`

The complete guide to **who-can-do-what** in a cellar app. For frontend devs and
agents: this is what decides whether a request gets data or a `403`, what rows a user
can see, and which roles exist. Companion to [`frontend-guide.md`](frontend-guide.md)
(the client/HTTP contract) and [`cellar-sync-design.md`](cellar-sync-design.md) (sync).

Source of truth: `src/engine/policy.c`. Everything below is how the engine actually
behaves, not a spec we hope it follows.

---

## 0. TL;DR

- Authorization is **deny-by-default**. A request is allowed only if the policy says so.
- A `policies.json` lives in the app bundle: `<app>/policies.json`. It's optional;
  without it the engine uses built-in role defaults.
- Authz is **per-table, per-action**: `list` / `get` / `create` / `update` / `delete`.
  You grant a *role* an *action* on a *table*.
- **Roles are just strings.** You define your own vocabulary (`owner`, `staff`,
  `kitchen`, `rider`, …) under `_roles`. There is **no limit** on how many.
- Each user has **exactly one** role (a string column on the user). No role inheritance,
  no role sets.
- The same policy gates **REST, sync (`/sync/pull` + `/sync/push`), and realtime**. A
  role with no access to a table can't read, sync, *or* subscribe to it.
- **Changing `policies.json` requires a server restart** to take effect (it's cached at
  app-open). This is different from `hooks.lua`, which hot-reloads.

---

## 1. The file

```
<app>/
  data.db        # tables → endpoints
  hooks.lua      # server-side behavior (hot-reloads on save)
  policies.json  # authorization (THIS doc) — read once, cached; restart to apply
  public/        # optional front-end
```

Shape:

```json
{
  "_default": "allow",
  "_roles": {
    "admin": { "superuser": true },
    "staff": { "self_register": true }
  },
  "_rpc": {
    "some_function": { "roles": ["staff"] }
  },
  "_session": {
    "strategy": "sliding",
    "ttl_seconds": 900,
    "absolute_max_seconds": 43200
  },

  "<table>": {
    "realtime": true,
    "list":   ["staff"],
    "get":    ["staff"],
    "create": ["staff"],
    "update": ["staff"],
    "delete": ["staff"]
  }
}
```

Top-level keys come in two kinds:
- **Reserved** (start with `_`): `_default`, `_roles`, `_rpc`, `_session`.
- **Table entries**: every other key is a table name; its value configures that table.

> ⚠️ **Applying changes.** `policies.json` is loaded the first time an app is served and
> cached for the life of the process (`cel_apps.c` caches the parsed policy per app).
> Edit it → **restart cellar**. (Hooks differ: `hooks.lua` is `stat`-checked and reloaded
> on mtime change, so it applies on next request. Policy does **not** do this.) If a
> policy change "isn't taking effect," you almost certainly need a restart.

If the file is absent, empty, or unparseable, the app falls back to the process default
policy (or the built-in role defaults). A parse error is logged.

---

## 2. The resolution algorithm (exact)

For a request `(table, action, role)`, the engine decides in this order
(`cel_policy_allows`):

1. **Superuser?** If the role is a superuser (see §4) → **ALLOW**, unconditionally.
   Skips every check below, including row ownership.
2. **Is there an explicit entry for this table?**
   - **Yes** → look at the action (`list`/`get`/…). It must be a JSON array of roles
     (or an object with a `"roles"` array). If the **role is in that array** → ALLOW,
     else → **DENY**. Crucially: if the action key is **missing** from a listed table,
     it's **DENY** — see §5, the fail-closed gotcha.
   - **No entry for the table** → go to step 3.
3. **No explicit table entry → consult `_default`:**
   - `_default: "allow"` → fall back to the role's default action set (`_roles[role].allow`,
     or the built-in default for `admin`/`editor`/`viewer`). A role with no `allow` set
     gets **nothing**.
   - `_default` absent or `"deny"` → **DENY**.

Mental shortcut: **a role can do an action on a table only if it's explicitly listed
there** (the `_default: "allow"` + `allow`-list path is the one exception, and only for
tables you didn't list at all).

---

## 3. Actions

The five actions map to HTTP like this:

| Action   | HTTP                                  | Notes |
|----------|---------------------------------------|-------|
| `list`   | `GET /api/<table>`                    | collection read; also gates `/sync/pull` for the table |
| `get`    | `GET /api/<table>/<id>`               | single-row read |
| `create` | `POST /api/<table>`                   | also gates `/sync/push` inserts |
| `update` | `PATCH/PUT /api/<table>/<id>`         | also gates `/sync/push` updates |
| `delete` | `DELETE /api/<table>/<id>`            | also gates `/sync/push` deletes (soft-delete for syncable tables) |

There is **no separate "sync" permission**. Sync and realtime reuse these same five.
A staff role that can `list`+`create`+`update`+`delete` `bookings` can fully sync
`bookings`; one that can only `list` gets a read-only mirror of it.

---

## 4. Roles (`_roles`)

A role is an **opaque string** stored on each user (`cel_users.role`, max 31 chars,
default `viewer`). The engine never hardcodes your vocabulary — "configure, don't fork."

Declare your roles under `_roles`. Each entry supports three optional fields:

```json
"_roles": {
  "owner":   { "superuser": true },
  "manager": { "allow": ["list","get","create","update"] },
  "clerk":   { "self_register": true },
  "kitchen": { }
}
```

| Field           | Effect |
|-----------------|--------|
| `superuser`     | `true` → role bypasses **all** policy checks and **all** row-ownership scoping. Use for owners/admins. |
| `allow`         | The role's **default action set** for tables that have **no explicit entry** — and only when `_default: "allow"`. An array of action names. Absent/empty ⇒ no default access. |
| `self_register` | `true` → users may sign themselves up with this role via `POST /auth/register` (see §8). Superuser roles can never self-register. |

### Built-in defaults

If you don't mention a role in `_roles`, it keeps a built-in default:

| Role             | Default |
|------------------|---------|
| `admin`          | superuser (everything) |
| `platform_admin` | superuser **always** — the global operator, created out-of-band; config can grant superuser to others but can never revoke it from `platform_admin` |
| `editor`         | all actions **except** delete (on un-entried tables, under `_default: allow`) |
| `viewer`         | read only — `list` + `get` (the column default for new users) |
| anything else / `anon` | nothing |

`_roles` entries are **additive/override**, never wholesale-replace: a role you don't
mention keeps its built-in default; one you do mention is fully governed by its entry
(e.g. `"admin": { "superuser": false }` *demotes* admin; `"editor": { "allow": [] }`
strips editor's defaults).

### One role per user

A user has a single role string — there are **no role sets and no inheritance**
("manager" does not automatically include "clerk"). If someone needs the union of two
roles, either:
- make a combined role (`manager_clerk`) and grant it everything both need, or
- make the broader role a strict superset and assign that.

A user's role is set at registration (`POST /auth/register` with `role`, gated by
`self_register`) or by an admin provisioning the account. It's returned in the login
response (`user.role`) so the client can branch its UI on it — but **never trust the
client's copy for security; the server re-checks every request.**

---

## 4b. Session policy (`_session`)

How long a login session lives, and whether it renews, is **per-app** config. Omit
`_session` entirely and you get the historical default: a **fixed 24h** session (no
renewal). The block selects a strategy and its parameters:

```jsonc
"_session": {
  "strategy": "fixed",          // "fixed" (default) | "sliding"
  "ttl_seconds": 86400,         // fixed: absolute lifetime. sliding: the IDLE window.
  "absolute_max_seconds": 0     // sliding only: hard cap measured from login; 0 = none
}
```

| Strategy  | Behavior |
|-----------|----------|
| `fixed`   | Absolute lifetime: the session expires `ttl_seconds` after login, regardless of activity. No renewal. (= the historical behavior, now per-app configurable.) |
| `sliding` | Idle window: each authenticated request that finds **less than half** the window left pushes the deadline out to `now + ttl_seconds` (lazy — at most ~one write per half-window). The session lives as long as it's used, then expires after `ttl_seconds` of inactivity. `absolute_max_seconds`, if set, is a hard ceiling from login that renewal can never exceed. |

Notes:
- **Defaults:** no block → `fixed`/24h. `strategy:"sliding"` with no `ttl_seconds` → 1h idle. A non-positive `ttl_seconds` (or negative cap) is ignored, keeping the default.
- **Unknown `strategy`** → logged as an error and falls back to the safe default (`fixed`); the app still serves.
- **Loaded once at startup** (like the rest of `policies.json`) — a change needs a cellar restart, not just a file copy.
- **Sliding × the session cache:** if `CEL_SESSION_CACHE_TTL` is set, a cache hit skips the DB (so it neither renews nor re-checks expiry until the cache entry lapses) → idle expiry is enforced only within ~the cache TTL. For tight idle enforcement keep the cache TTL small or off. Full design + the future stateless/JWT + external-store strategies: [`session-management.md`](session-management.md).

---

## 5. Table entries — and the fail-closed gotcha

A table entry lists, per action, the roles allowed:

```json
"bookings": {
  "realtime": true,
  "list":   ["staff","kitchen"],
  "get":    ["staff","kitchen"],
  "create": ["staff"],
  "update": ["staff"],
  "delete": ["staff"]
}
```

> ⚠️ **The gotcha: once a table lists *any* action, its *unlisted* actions are denied.**
> An entry that spells out even one action becomes an explicit allow-list — there is no
> "and everything else is allowed." So `"bookings": { "list": ["staff"] }` lets staff
> **read** but denies create/update/delete. List every action × role you want.
>
> **Meta-only entries are exempt (fixed).** A table entry that lists **no action at all**
> — e.g. just `{ "realtime": true }` — is treated like an *unlisted* table: it falls
> through to `_default` instead of denying every CRUD action. So adding `realtime` to opt
> a table into change events **no longer silently locks it.** (Earlier this was the #1
> footgun; the engine now distinguishes a metadata-only entry from an explicit allow-list
> — see `policy.c: table_lists_any_action`.) The moment you add *one* action key, the
> table is back in explicit-allow-list mode and unlisted actions deny.

The corollary is the clean part: **you only ever write grants, never denials.** Leaving
a role out of an action array *is* the denial. A tightly-scoped role is often *less*
JSON than a broad one (see §7 recipes).

### `realtime`

`"realtime": true` opts the table into live WebSocket change events. Deny-by-default:
no flag ⇒ no realtime for that table. Subscribing still requires `list` permission on
the table, so realtime never leaks rows a role couldn't already read.

---

## 6. Row-level ownership (per-row scoping)

So far, permissions are table-wide: if you can `list` a table you see *all* its rows.
To restrict a role to **its own rows**, add an ownership spec to the action. It's
configured by replacing the role array with an **object** `{ "roles": [...], <scope> }`.

Three scope kinds:

```json
"notes": {
  "list":   { "roles": ["user"], "owner_column": "user_id" },
  "get":    { "roles": ["user"], "owner_column": "user_id" },
  "create": { "roles": ["user"], "owner_column": "user_id" },
  "update": { "roles": ["user"], "owner_column": "user_id" },
  "delete": { "roles": ["user"], "owner_column": "user_id" }
}
```

| Scope | Config | Meaning |
|-------|--------|---------|
| **EQ**  | `"owner_column": "user_id"` | the row's `user_id` must equal the caller's user id |
| **OR**  | `"owner_any": ["author_id","assignee_id"]` | caller matches **any** of these columns (e.g. rows you wrote *or* are assigned) |
| **VIA** | `"owner_via": { "table": "memberships", "ref": "org_id", "local": "org_id", "user": "user_id" }` | caller must be a member in a **related** table — here: rows whose `org_id` matches an `org_id` in `memberships` where `user_id` = caller |

### How ownership behaves (important for clients)

- **On reads (`list`)** ownership is a **filter**, not an error. Rows you don't own
  simply **don't appear** in the response. You get `200` with fewer rows, never a `403`.
  (`get`/`update`/`delete` on a specific non-owned row → it's invisible, so `404`/`403`.)
- **On `create`** the **EQ** owner column is **forced** to the caller's id server-side —
  the client can't spoof ownership; whatever `user_id` you send is overwritten.
- **OR and VIA cannot be enforced on `create`** (there's no single column to force).
  Configuring them on `create` is a **misconfiguration** → the request fails `500`. Use
  EQ for `create`, and OR/VIA for the read/update/delete actions if needed.
- **Superusers are never row-scoped** — they see and edit every row.

> For a "shared org, no per-user ownership" app (like ClerkHalls — all staff share the
> org's data), **don't use owner scoping at all.** Plain role arrays give every staff
> member the whole dataset, which is what a shared back-office wants.

---

## 7. `_default` — the global fallback

`_default` decides what happens for a table that has **no explicit entry**:

```json
"_default": "allow"     // un-entried tables fall back to role defaults (built-in / _roles.allow)
"_default": "deny"      // un-entried tables are denied (also the behavior if _default is absent)
```

Object forms also work: `{ "allow": true }`, `{ "deny": true }` (and `{ "deny": false }`
== allow). Anything unrecognized, or omitting `_default` entirely, **fails closed**
(deny) — and the engine logs a warning at load so you notice.

Two coherent strategies:

- **Whitelist (recommended for most apps):** omit `_default` (or set `"deny"`), then
  explicitly list every table you expose. Anything you forget to list is denied — safe
  by construction. New tables are invisible until you grant them.
- **Permissive base:** `_default: "allow"` plus per-role `allow` lists, then list only
  the tables that need *tighter-than-default* rules. Riskier: a new table you add to the
  schema is immediately reachable by any role whose `allow` covers the action. Only use
  this if you understand that a forgotten table is **open**, not closed.

> ClerkHalls uses `_default: "allow"` but explicitly lists **every** table, so the
> permissive path never actually fires — every table's access is decided by its own
> entry. That's a fine belt-and-suspenders setup.

---

## 8. Scoping a role to one or two tables

A very common need: a role that can only touch a subset of tables (e.g. `kitchen` sees
only `bookings` + `menu_items`). This is the **natural** case, not a special feature.

The recipe: give the role **no `allow` set** (so it has zero default access), then name
it **only** in the tables you want.

```json
"_roles": {
  "admin":   { "superuser": true },
  "staff":   { "self_register": true },
  "kitchen": { "self_register": true }      // no "allow" → zero access by default
},

"bookings":   { "realtime": true, "list": ["staff","kitchen"], "get": ["staff","kitchen"],
                "create": ["staff"], "update": ["staff"], "delete": ["staff"] },
"menu_items": { "realtime": true, "list": ["staff","kitchen"], "get": ["staff","kitchen"],
                "create": ["staff"], "update": ["staff"], "delete": ["staff"] },

"payments":   { "list": ["staff"], "get": ["staff"], "create": ["staff"], "update": ["staff"], "delete": ["staff"] },
"venues":     { "list": ["staff"], "get": ["staff"], "create": ["staff"], "update": ["staff"], "delete": ["staff"] }
// ...every other table: kitchen simply isn't listed → denied
```

`kitchen` can read those two tables and nothing else — not even create/update on them,
and no access to the other tables. Because authz gates sync too, a `kitchen` user's
offline mirror will contain **only** `bookings` + `menu_items`.

---

## 9. RPC functions (`_rpc`)

If your `hooks.lua` exposes custom RPC functions (callable endpoints beyond table CRUD),
they're **deny-by-default and must be whitelisted** under `_rpc`:

```json
"_rpc": {
  "request_ride":  { "roles": ["rider"] },
  "close_books":   { "roles": ["manager","owner"] }
}
```

- A function **not** listed under `_rpc` cannot be called by **anyone — including
  superusers** (the whitelist is the exposure gate; an un-whitelisted function is simply
  not reachable over the API).
- For a whitelisted function, a **superuser bypasses** the per-function `roles` list;
  other roles must appear in it.

---

## 10. Self-service registration

`POST /auth/register` lets a user create their own account. It's gated:

- A role can be obtained via signup **only** if its `_roles` entry has
  `"self_register": true` **and** it is not a superuser (superuser roles are
  provisioned out-of-band only — the boundary that stops signup from escalating to
  admin).
- If the register request omits a role, the engine picks the **first** self-registerable
  role in `_roles` as the default.
- If no role is self-registerable, registration is refused (`403`).

For production you typically turn `self_register` **off** and have an admin create staff
via the admin user-creation endpoint. For local testing it's convenient to leave on.

---

## 11. What the client sees, and how to handle it

| Status | Meaning | Client action |
|--------|---------|---------------|
| `200` + fewer rows | owner scoping filtered out rows you don't own | normal — render what you got |
| `401 unauthorized` | missing/expired/invalid token | drop to login, re-authenticate, retry |
| `403 forbidden` | policy denied this `(table, action, role)` | not retryable as-is; the role lacks permission. Hide/disable the UI affordance for this role |
| `403` on `/sync/push` | one mutation in the batch hit a policy deny | the **whole batch** is rejected (all-or-nothing). Don't queue actions the user's role can't perform |

Guidance for frontend/agents:

- **Branch UI on `user.role`** (from the login response) to hide buttons a role can't
  use — but treat that purely as UX. The server is the only authority; a `403` is the
  real boundary.
- **Don't enqueue forbidden mutations** in an offline queue. Because `/sync/push` is
  all-or-nothing, a single forbidden mutation wedges the entire queue until it's removed.
  Gate write affordances by role *before* they reach the queue.
- A `403` on a table you expected to read usually means the policy **listed the table
  but not your action** (the §5 gotcha) — check the table entry spells out your action
  for your role.

---

## 12. Checklist

When writing or reviewing a `policies.json`:

- [ ] Every table you intend to expose has an entry (or `_default: "allow"` is set
      deliberately and you understand un-listed tables fall back to role defaults).
- [ ] A table that lists **any** action lists **every** action × role you mean to allow
      (unlisted actions deny). A `realtime`-only entry is fine on its own — it falls
      through to `_default` (§5).
- [ ] Each custom role is declared in `_roles`; roles you want scoped have **no `allow`**.
- [ ] Self-registerable roles are intentional (`self_register: true`), and no superuser
      role is self-registerable.
- [ ] `create` actions use `owner_column` (EQ) if row-scoped — never `owner_any`/`owner_via`.
- [ ] Any custom RPC function is whitelisted under `_rpc`.
- [ ] You **restarted cellar** after editing the file.

---

## Appendix — full annotated example (ClerkHalls)

```json
{
  "_comment": "One bundle = one organization; staff share the org's data (no per-row ownership).",
  "_default": "allow",
  "_roles": {
    "admin": { "superuser": true },          // owner — everything
    "staff": { "self_register": true }        // shared back-office access
  },

  // Each table: realtime on, full staff CRUD spelled out (because a listed table
  // fail-closes its unlisted actions — §5).
  "organization":            { "realtime": true, "list": ["staff"], "get": ["staff"], "create": ["staff"], "update": ["staff"], "delete": ["staff"] },
  "venues":                  { "realtime": true, "list": ["staff"], "get": ["staff"], "create": ["staff"], "update": ["staff"], "delete": ["staff"] },
  "halls":                   { "realtime": true, "list": ["staff"], "get": ["staff"], "create": ["staff"], "update": ["staff"], "delete": ["staff"] },
  "bookings":                { "realtime": true, "list": ["staff"], "get": ["staff"], "create": ["staff"], "update": ["staff"], "delete": ["staff"] }
  // ...remaining tables identical.
}
```

To add a read-only `kitchen` role that only sees `bookings` + `menu_items`, see §8.
