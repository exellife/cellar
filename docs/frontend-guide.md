# Building a front-end (or agent) on cellar

This is the practical guide for anyone — a human dev or a coding agent — building a
client against a cellar app: a web SPA, a Flutter/mobile app, or a script.

cellar is a JSON/REST backend with opaque-token auth. The realtime WebSocket, the
offline-first **sync** API, and the static-file serving are **optional** layers you
opt into; the REST API is the whole contract.

---

## 1. The mental model

A cellar **app** is a self-contained bundle directory:

```
<app>/
  data.db        # your schema + data (SQLite). Tables you make become REST endpoints.
  hooks.lua      # optional server-side behavior (custom endpoints, validation, …)
  public/        # optional: your front-end (HTML/CSS/JS/images/fonts), served as a site
  policies.json  # optional: who-can-do-what (authorization)
```

One cellar process can host many apps, routed by the **Host header** (`a.com`,
`b.com`). Each app is isolated — its own database, behavior, front-end, and authz.

Create one with the CLI (the operator does this):

```sh
CEL_APPS_DIR=./apps cellar provision shop.example admin@shop.example 's3cret-pw'
# -> apps/shop.example/{data.db, hooks.lua, public/index.html}  + a seeded admin
```

Then define your tables in `apps/shop.example/data.db` with any SQLite tool — a
table called `products` immediately becomes `/api/products`.

### Running it locally (and testing several apps at once)

In multi-app mode the **Host header picks the app**, so you can test against a local
cellar with no DNS setup:

```sh
CEL_APPS_DIR=./apps cellar provision shop.local admin@shop 's3cret-pw'
CEL_APPS_DIR=./apps CEL_PORT=8090 cellar          # one process serves every app
```

- **From code / curl / an agent** — just override the Host; nothing else changes
  between apps:
  ```sh
  curl -H "Host: shop.local" http://127.0.0.1:8090/api/products
  ```
- **In a browser** the name must resolve. Either add `127.0.0.1  shop.local` to
  `/etc/hosts`, or use a wildcard-localhost domain like **`*.lvh.me`** (it publicly
  resolves to `127.0.0.1`) — provision apps as `shop.lvh.me` / `blog.lvh.me` and open
  `http://shop.lvh.me:8090/` directly. Run several apps side by side, one Host each.

Each app is **fully isolated**: separate users, sessions, data, and authz — a
token from one Host is rejected on another.

---

## 2. The HTTP API

Base URL is the app's origin. Every authenticated request carries
`Authorization: Bearer <token>`. All bodies and responses are JSON. Errors are
`{ "status": "error", "message": "..." }` with a matching HTTP status.

### Auth

| Call | Body | Returns |
|---|---|---|
| `POST /auth/login` | `{email, password}` | `{token, user:{id,email,role,…}}` — `token` is a 64-char bearer |
| `POST /auth/register` | `{email, password, role?}` | `202` (self-service signup; gated by policy) |
| `POST /auth/password/change` | `{current_password, new_password}` (Bearer) | `200` — in-session change; `401` if the current password is wrong. No email round-trip; the session stays valid. |

```js
const r = await fetch("/auth/login", {
  method: "POST", headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ email, password }),
});
const { token, user } = await r.json();   // store `token`; send it as Bearer
```

Tokens are **opaque server-side sessions** (not JWTs). Lifetime + renewal are
**per-app policy** (`_session` in `policies.json` — fixed/sliding; default fixed
24h; see [`policy-guide.md`](policy-guide.md)). Store the token (localStorage on web,
secure storage on mobile) and re-login on a `401`. **There is no REST logout
endpoint**: drop the token client-side; the session expires by policy (operators can
force-revoke server-side).

#### Device tokens — PIN fast-login (opt-in)

When the app enables it (`_session.device_ttl_seconds > 0`), a client can mint a
**long-lived, revocable device token**, store it (e.g. encrypted behind a PIN), and
exchange it for a fresh session on unlock — so the user never re-types the password,
yet the raw password is never stored on the device and the operator can revoke a lost
device. (If disabled, these endpoints return `404`.)

| Call | Body | Returns |
|---|---|---|
| `POST /auth/device` | `{label?}` (Bearer) | `201 {id, device_token}` — **shown once**; store `device_token` (e.g. PIN-encrypted) |
| `POST /auth/session/from-device` | `{device_token}` | `200 {token, user}` — a normal session; `401` if bad/expired/revoked. (No MFA step — the enrolled device is the possession factor.) |
| `GET /auth/devices` | (Bearer) | `200 {devices:[{id,label,created_at,last_used_at,expires_at}]}` — the caller's own (never the token value) |
| `POST /auth/devices/revoke` | `{id}` (Bearer) | `200`; `404` if not the caller's device |

The device token is **static/reusable** (exchange as often as you like). It's hashed
at rest, and a full account recovery — password reset, admin `set_password`, or
"log out everywhere" — **revokes all of the user's device tokens**. Flow: after a
normal login, `POST /auth/device` once and stash the token under the PIN; on each
unlock, `POST /auth/session/from-device` to get a session.

#### Push notifications (web-push)

For off-site delivery (reaching a user who isn't in the app), the engine stores
web-push subscriptions and fans notifications out to them. **Client side:** register
a service worker, ask for push permission, get a `PushSubscription`, and POST it:

| Call | Body | Returns |
|---|---|---|
| `POST /push/subscribe` | `{endpoint, keys:{p256dh, auth}, ua?}` (Bearer) | `201 {id}` — upsert by `endpoint` |
| `POST /push/unsubscribe` | `{endpoint}` or `{id}` (Bearer) | `200`; `404` if not yours |
| `GET /push/subscriptions` | (Bearer) | `200 {subscriptions:[{id,endpoint,ua,created_at,last_used_at}]}` — caller's own, no key material |

The `{endpoint, keys}` come straight from the browser's `PushSubscription.toJSON()`.
Re-subscribing the same endpoint refreshes the keys (idempotent). A full account
recovery (password reset / `set_password` / log-out-everywhere) drops the user's
device tokens; subscriptions are pruned when the push service reports them gone.

> **Status:** subscription storage + endpoints are **live now** — you can build the
> subscribe flow. **Actual push delivery is not wired yet** (the VAPID sender is the
> next backend slice), so a subscription won't receive messages until then. In-app
> notifications (the realtime feed) already work today.

Also available when the app enables them: `POST /auth/oauth` (OIDC sign-in),
`POST /auth/password/forgot` + `/auth/password/reset`, the email-verification code
flow (below), and the `POST /auth/mfa/*` (TOTP) flow. `POST /auth/users` (Bearer,
**superuser**) creates users out-of-band.

### Email verification — 6-digit code (soft, non-blocking)

Modern signup: **register auto-logs-in immediately** (you get a session token), and a
**6-digit code** is emailed in the background (when the app's mailer is on). Verification
is *non-blocking* — the user is already in; verifying just flips `email_verified` (which
apps use for progressive trust / higher limits). Mobile-friendly: the code is typed in-app,
so you stay in one session (OS one-time-code autofill), no cross-browser magic link.

- **`user.email_verified`** — a boolean on the register/login user object.
- **`POST /auth/verify-email { code }`** — **authenticated** (send the Bearer session from
  register/login; the session identifies the user). `200` on success → `email_verified`
  becomes true; `400` on a wrong/expired code (show "wrong or expired — resend"); `429` if
  hammered. The code is **single-use**, TTL **15 min**, and **capped at 5 wrong attempts**
  (after which it's dead — request a new one).
- **`POST /auth/verify-email/resend`** — (Bearer) re-sends a fresh code; always `200`
  (no-op if already verified). A **server-side ~60s cooldown** rate-limits it, so a client
  timer isn't the only guard.
- Already-verified (or an OAuth signup, which arrives pre-verified) → no code is sent.
- The email body/subject are localizable/brandable per app via the `render_email`
  hook (jarchy ships a **RU** template carrying the code) — see `docs/policy-guide.md`.

**Letting a non-superuser role create accounts** (e.g. a `manager` onboarding a
`clerk`): `POST /auth/users` is superuser-only, so do it from a hook instead. The
`cellar.create_user(email, password, role) → id, err` Lua primitive mints a login
(a password identity) and returns the new user id; your `hooks.lua` `rpc` enforces
who-may-create-whom. The engine refuses `platform_admin`; everything else is the
bundle's policy. Example — a manager mints clerks only, then writes the roster row:

```lua
if name == 'create_clerk' then
  if who.role ~= 'manager' and who.role ~= 'admin' then return nil, 'forbidden' end
  local id, err = cellar.create_user(args.email, args.password, 'clerk')  -- role forced
  if not id then return nil, err end
  cellar.exec('INSERT INTO staff_profiles(id, email, role) VALUES (?,?,?)',
              { id, args.email, 'clerk' })
  return { id = id }
end
```

**Resetting an existing user's password without email** (an admin "set a temp
password" action when `POST /auth/password/forgot` isn't usable because no mailer is
configured): `cellar.set_password(email, new_password) → true, err` is the
update-side sibling of `create_user`. It sets the password on an existing `password`
account with **no current-password check** (so the rpc MUST authorize it) and
revokes the target's sessions + pending MFA. The engine **refuses resetting a
superuser** (admin-takeover floor); the bundle enforces who-may-reset-whom among the
rest — read the target's role with `cellar.query` (resetting a temp password *up* the
ladder would leak it, so only reset at/below the caller). The user then sets their
own password via the authenticated `POST /auth/password/change`. Both primitives are
**rpc-only** (they take the write lock; calling them from `before`/`after`/`resolve`/
`job` returns an error rather than deadlocking).

```lua
if name == 'reset_password' then
  local target = cellar.query('SELECT role FROM cel_users WHERE email = ?1', { args.email })[1]
  if not target then return nil, 'no such user' end
  -- only an admin may reset, and never another admin (temp pw would be exposed up-ladder)
  if who.role ~= 'admin' or target.role == 'admin' then return nil, 'forbidden' end
  local ok, err = cellar.set_password(args.email, args.new_password)
  if not ok then return nil, err end
  return { ok = true }
end
```

### Reading data — `GET /api/<table>`

Returns `{ rows: [...], count }`. Query params:

| Param | Example | Effect |
|---|---|---|
| filter | `?in_stock=gt.200` | `col=op.value` (`col=value` is `eq` shorthand). ops: `eq, neq, gt, gte, lt, lte, like, ilike`; **in**: `?status=in.a,b,c` (PostgREST's `in.(a,b,c)` parens also accepted); **null**: `?ref=is.null` / `?ref=is.not.null` |
| `where` | `?where=<urlencoded JSON>` | boolean trees: `{"or":[{"price":{"lt":5}},{"price":{"gt":50}}]}`, `and`, `not`, `between` |
| `select` | `?select=name,price` | project columns |
| `order` | `?order=-price` | sort (`-` = desc) |
| `limit` / `cursor` | `?order=sku&limit=20&cursor=<next_cursor>` | keyset pagination (response has `next_cursor`) |
| `count` | `?count=exact` | adds `total` (unpaginated) |
| `embed` | `?embed=categories` | inline a related row/array — follows FKs **both ways**: forward (a row's parent, e.g. `bookings?embed=halls`) and reverse/to-many (a row's children, e.g. `bookings?embed=payments`). Nests: `embed=products.categories`. **List-only** — for one row + its relations use `?id=eq.<id>&embed=…`, not `GET /api/<table>/<id>` (which ignores `embed`). |
| `group` / `aggregate` | `?group=category_id&aggregate=count,sum:price,avg:price` | rollups |

`GET /api/<table>/<id>` → `{ row }` (or `404`).

### Writing data

| Call | Body | Returns |
|---|---|---|
| `POST /api/<table>` | the row's columns, e.g. `{name, sku, price}` | `201 {row}` |
| `PATCH /api/<table>/<id>` | changed columns | `200 {row}` |
| `DELETE /api/<table>/<id>` | — | `200` |

### Custom endpoints — `POST /rpc/<name>`

Anything beyond CRUD is an `rpc` defined in the app's `hooks.lua`. Body is the
args; response is `{status:"ok", result:<json>}`.

```js
await fetch("/rpc/checkout", {
  method: "POST",
  headers: { "Content-Type": "application/json", "Authorization": "Bearer " + token },
  body: JSON.stringify({ cart_id: 42 }),
});
```

### Discovering the schema

- `GET /schema` → tables + columns + types (auth-gated).
- `GET /openapi.json` → a full OpenAPI 3.0 doc — **codegen a typed client** from it
  (e.g. a Dart/TS client) instead of hand-writing requests.

---

## 3. Status codes you'll see

`200/201/202` ok · `400` bad request · `401` not authenticated (or bad/expired
token) · `403` forbidden (policy) · `404` not found / unknown table / unknown app
· `409` conflict (unique violation) · `413` body too large · `429` rate-limited.

Error messages are intentionally generic (no schema leakage) — don't parse them
for logic; branch on the status code.

A `403` means the app's **authorization policy** denied this role/action/table — it's
not retryable as-is. See [`policy-guide.md`](policy-guide.md) for the full model
(roles, per-table grants, row-ownership, and how to branch your UI on `user.role`).

---

## 4. The front-end as a bundle (`public/`)

For a **web** app, drop your built site into the app's `public/`:

```
apps/shop.example/public/
  index.html        # served at /  (and as the SPA fallback)
  assets/app.js
  assets/app.css
```

cellar serves `public/` for the app's Host, with an **SPA fallback**: any GET that
isn't a real file serves `index.html`, so client-side routes (`/admin`,
`/profile/:id`) work — your JS then calls `/api`, `/rpc`, `/auth`. The API routes
are never shadowed. So a single-page app "just works": ship static files, talk to
the API.

A **Flutter/native** app doesn't use `public/` at all — it's a client hitting the
REST API directly. (CORS is a browser concern; it does **not** apply to native
mobile. For Flutter *web*, the operator enables CORS.)

### What the bundle actually contains (it's more than `public/`)

`public/` is just the front-end slice. A full **app bundle** is the server-side
contract too — and once an app is deployed, **these are yours to evolve**:

```
<app>/                  # the deployable bundle you build + hand to the platform
├── public/             # your built SPA (index.html + hashed assets)
├── schema.sql          # the data model (tables)
├── migrations/         # ordered schema deltas — see "Schema changes" below
├── hooks.lua           # server-side behavior (validation, rpc, sync resolve)
└── policies.json       # role grants per table
                        # (data.db is created on the server, never shipped)
```

Full spec: [`app-bundle.md`](app-bundle.md). Keep `public/` **same-origin** — the
client's `baseUrl` defaults to `""` so `/auth` `/api` `/rpc` `/sync` are relative;
that makes the one bundle portable across domains (localhost → prod) with no rebuild.

**Build:** produce a self-contained bundle dir — build the SPA into `public/`, then
assemble it with `schema.sql` / `migrations/` / `hooks.lua` / `policies.json`. Ship
that; the platform provisions/deploys it. Do **not** ship a `data.db`.

### Schema changes — use migrations, never a wipe

After an app holds real data, you can't just re-run `schema.sql` (it's create-only;
it can't alter an existing table) and you must not wipe the DB. **Every schema change
is a new forward-only file** `migrations/NNNN_name.sql`; the platform runs
`cellar migrate <app>` on deploy (it backs up first, applies pending migrations in
order, with the app stopped). Rules: monotonic `NNNN_` prefix, **never edit a
migration once applied** (checksummed — fix-forward with a new file), plain SQL (no
`BEGIN`/`COMMIT`). The first one (`0001_init.sql`) uses `CREATE … IF NOT EXISTS` so it
adopts already-deployed DBs as a no-op. A change to a **syncable** table (one with
`rev`/`deleted`) is also a *client* local-schema change — roll out additive-first.
Full how-to: [`app-bundle.md` §4](app-bundle.md) (+ the ClerkHalls
`MIGRATIONS.md` worked example).

---

## 5. Realtime (optional)

cellar can push live `CHANGE` events over a WebSocket when a row a client is
allowed to see is created/updated/deleted. It's an additive feature — for many
apps, plain REST (fetch/poll) is enough. Reach for it only when you need live
updates; pure-REST clients ignore it entirely.

**Connect.** Open a WebSocket to the **same origin** as the API (`ws(s)://<host>/`).
It's **Host-routed like the HTTP API**, so in dev proxy it too — add the WS to your
proxy with `ws: true` so the `Host` header still says the app host (e.g. `localhost`):

```ts
// vite.config.ts — alongside the /api, /auth, /rpc, /media entries
proxy: { "/ws": { target: "ws://127.0.0.1:8080", ws: true, changeOrigin: true,
                  headers: { Host: "localhost" }, rewrite: p => p.replace(/^\/ws/, "/") } }
// then in the client: new WebSocket(`ws://${location.host}/ws`)
```

**Wire protocol.** Each message is a **binary** WS frame whose payload is an 8-byte
big-endian header + a JSON body:

```
byte 0    opcode   (u8)   0x10 LOGIN · 0x20 SUBSCRIBE · 0x22 CHANGE
byte 1    flags    (u8)   0
bytes 2-3 mid      (u16)  client message id (echoed in the reply)
bytes 4-7 len      (u32)  JSON body length
bytes 8…  body     JSON (len bytes)
```

**Flow:** `LOGIN → SUBSCRIBE (per table/scope) → receive CHANGE frames.`

| op | send | reply |
|---|---|---|
| `LOGIN` `0x10` | `{email, password}` | `{token, …}` (a normal session token) |
| `SUBSCRIBE` `0x20` | `{token, table, key?: {column, value}}` | `{status:"ok"}` or a denial |
| `CHANGE` `0x22` | — (server push) | `{table, op, row}` when a visible row changes |

**Authorization is enforced on `SUBSCRIBE`** — you only receive changes for rows the
table's policy would let you read (owner-scope / `owner_via` membership). A subscribe
outside your scope is refused (`status` ≠ `"ok"`), so realtime never leaks.

### Classifieds subscriptions

- **Chat messages (replaces polling):** subscribe to a conversation's messages —
  `SUBSCRIBE {token, table:"message", key:{column:"conversation_id", value:<conversation_id>}}`.
  You then get a `CHANGE {table:"message", row:{…the new message…}}` each time either
  participant sends. **Only the two participants may subscribe** (`conversation_member`
  membership); a non-member is denied. Open one subscription per open thread.
- **Notification bell:** `SUBSCRIBE {token, table:"notification"}` (no `key` — it's
  owner-scoped to the logged-in user). You get a `CHANGE {table:"notification", row}`
  for each new notification (new message, saved-search match, listing expiry…).

  The bell's read/manage side is **RPCs**, not `/api/notification` (that table is
  admin-only for direct access; use these — all owner-scoped, role `user`):
  - `POST /rpc/notifications {unread_only?}` → `{ notifications:[{id,type,title,body,subject_id,read_at,created_at}] }`
  - `POST /rpc/unread_count` → `{ unread:N }`
  - `POST /rpc/mark_read {id?}` → `{ unread:N }` — **omit `id` = mark ALL read** (clears the badge); pass `id` = that one.

  **Notification `type` → `subject_id` (deep-link routing):** `message` → `subject_id` is the
  conversation id; `saved_search` → the matching listing id; `listing_expired` → the listing id.

Both are proven by the engine's `classifieds_chat_ws` e2e (live delivery to a
participant + a non-member denied + a live notification). Fall back to a one-shot
refetch on reconnect to backfill anything missed while disconnected.

---

## 6. Offline-first sync (optional)

For apps that must **work offline** and sync across devices (a notes app, a field-data
tool, a mobile client on flaky networks), cellar has a delta-sync API on top of the
normal write path. It's opt-in per table and additive — a purely online app ignores it.

**A table is *syncable* when its schema declares two extra columns: `rev INTEGER` and
`deleted INTEGER`** (ask the operator/backend to add them). The engine then stamps a
monotonic `rev` on every write and turns `DELETE` into a soft-delete (a tombstone), so a
device that was offline can later learn a row changed or went away.

Two endpoints — both `POST`, bearer-authed, owner-scoped like the rest of the API:

| Call | Body | Returns |
|---|---|---|
| `POST /sync/pull` | `{ since, device_id?, tables?, limit? }` | `{ changes:{ table:[rows] }, cursor, more }` — rows **include tombstones** (`deleted:1`) so deletions propagate |
| `POST /sync/push` | `{ mutations:[ {op:"put"\|"del", table, id, base_rev?, values?, mutation_id?} ], device_id? }` | `{ results:[ {id, status:"applied"\|"conflict", winner?, rev, deduped?} ], cursor }` |

The client loop (see the full reference client below):

1. **Local mirror.** Keep a local copy of the rows (SQLite on mobile, IndexedDB/memory on web).
2. **Offline writes queue.** Each create/edit/delete becomes a pending mutation. Mint the
   row `id` **client-side** (a UUID) so offline creates never collide. Attach a unique
   `mutation_id` per mutation, and `base_rev` = the last server-confirmed `rev` of the row.
3. **`sync()` = push then pull.** Push the queue, then pull everything `since` your stored
   `cursor` and apply it (the **server is authoritative** for synced state). Send a stable
   `device_id` on both so the server can GC tombstones you've already seen.
4. **Realtime is the online fast-path.** While connected, also apply `CHANGE` events (§5) so
   peers' writes — including those that arrived via `/sync/push` — show up live.

**Conflicts** (someone else changed the row since your `base_rev`): resolved **last-write-wins**
by default; an app can override per-table with a `resolve(table, incoming, current, who)` hook
(e.g. "most-recent edit wins", "higher quantity wins"). The result tells you `winner`
(`incoming`/`server`); a `sync_pull` then brings the canonical row.

**Idempotent retries:** re-pushing a mutation with the same `mutation_id` is a no-op
(`deduped:true`) — so a reconnect that re-sends the queue can't double-apply or lose another
device's write. Two caveats: the push response `cursor` is **informational** — advance your
stored cursor only from a `pull` response; and tombstones are reclaimed out-of-band by the
operator (`cellar sync-gc`), once every device has pulled past them.

**Reference client:** [`examples/offline_notes/`](../examples/offline_notes/) is a complete,
runnable web client — `public/sync.js` is the whole offline loop (local store, queue,
push/pull, realtime, conflict handling) in ~150 lines. Crib from it.

---

## 7. What you can ask the backend to do (hooks)

When the front-end needs server-side logic, that lives in the app's `hooks.lua`
(the operator/backend writes it). As a front-end dev/agent, you can rely on — and
request — these:

- **`rpc(name, args, who)`** → a custom `POST /rpc/<name>` endpoint returning JSON.
- **`before(op, table, input, who)`** → validate/transform a create/update before it
  hits the DB (e.g. force `owner_id`, normalize fields, reject bad input → `400`).
- **`authorize(op, table, row, who)`** → an extra allow/deny gate beyond the policy.
- **`after(op, table, row, who)`** → post-commit side effects (audit, notify).
- **`resolve(table, incoming, current, who)`** → the sync conflict rule (§6): pick
  `'incoming'` or `'current'` per syncable table.

So "this field should be server-set", "this action needs a custom rule", "call
this side-effect on create", or "this is how conflicts resolve" are all backend hook
changes, not front-end hacks.

---

## 8. Quick checklist

- [ ] Get the app's origin and (for multi-app) confirm the Host routes to your app.
- [ ] `POST /auth/login` → keep the bearer token; send it on every call.
- [ ] Model your screens on `GET /api/<table>` (filter/select/order/embed/paginate).
- [ ] CRUD via `POST/PATCH/DELETE /api/<table>[/<id>]`.
- [ ] Anything non-CRUD → ask for an `rpc` hook; call `POST /rpc/<name>`.
- [ ] Offline-first? Use `/sync/pull` + `/sync/push` (table needs `rev`+`deleted`);
      start from the `examples/offline_notes/` client.
- [ ] Web SPA → build into `public/`; native → hit the API directly.
- [ ] Generate a client from `GET /openapi.json` if you want types.
- [ ] Hitting `403`s, or need roles/per-row access? Read [`policy-guide.md`](policy-guide.md).
