# Building a front-end (or agent) on cellar

This is the practical guide for anyone — a human dev or a coding agent — building a
client against a cellar app: a web SPA, a Flutter/mobile app, or a script.

cellar is a JSON/REST backend with opaque-token auth. The realtime WebSocket and
the static-file serving are **optional** layers you opt into; the REST API is the
whole contract.

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

```js
const r = await fetch("/auth/login", {
  method: "POST", headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ email, password }),
});
const { token, user } = await r.json();   // store `token`; send it as Bearer
```

Tokens are **opaque server-side sessions** (not JWTs), 24h TTL. Store the token
(localStorage on web, secure storage on mobile) and re-login on a `401` — there's
no refresh-token dance. **There is no REST logout endpoint**: drop the token
client-side; the session expires by TTL (operators can force-revoke server-side).

Also available when the app enables them: `POST /auth/oauth` (OIDC sign-in),
`POST /auth/password/forgot` + `/auth/password/reset`, `POST /auth/verify-email`,
and the `POST /auth/mfa/*` (TOTP) flow. `POST /auth/users` (Bearer, admin) creates
users out-of-band.

### Reading data — `GET /api/<table>`

Returns `{ rows: [...], count }`. Query params:

| Param | Example | Effect |
|---|---|---|
| filter | `?in_stock=gt.200` | `col=op.value` (`col=value` is `eq` shorthand); ops: `eq, neq, gt, gte, lt, lte, like, ilike, in` |
| `where` | `?where=<urlencoded JSON>` | boolean trees: `{"or":[{"price":{"lt":5}},{"price":{"gt":50}}]}`, `and`, `not`, `between` |
| `select` | `?select=name,price` | project columns |
| `order` | `?order=-price` | sort (`-` = desc) |
| `limit` / `cursor` | `?order=sku&limit=20&cursor=<next_cursor>` | keyset pagination (response has `next_cursor`) |
| `count` | `?count=exact` | adds `total` (unpaginated) |
| `embed` | `?embed=categories` | inline a related row/array (follows foreign keys; `embed=products.categories` nests) |
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

---

## 5. Realtime (optional)

cellar can push live `CHANGE` events over a WebSocket when a row a client is
allowed to see is created/updated/deleted. It's an additive feature — for many
apps, plain REST (fetch/poll) is enough. The protocol is a small binary opcode
frame (login → subscribe → receive changes); reach for it only when you need live
updates. Pure-REST clients ignore it entirely.

---

## 6. What you can ask the backend to do (hooks)

When the front-end needs server-side logic, that lives in the app's `hooks.lua`
(the operator/backend writes it). As a front-end dev/agent, you can rely on — and
request — these:

- **`rpc(name, args, who)`** → a custom `POST /rpc/<name>` endpoint returning JSON.
- **`before(op, table, input, who)`** → validate/transform a create/update before it
  hits the DB (e.g. force `owner_id`, normalize fields, reject bad input → `400`).
- **`authorize(op, table, row, who)`** → an extra allow/deny gate beyond the policy.
- **`after(op, table, row, who)`** → post-commit side effects (audit, notify).

So "this field should be server-set", "this action needs a custom rule", or "call
this side-effect on create" are all backend hook changes, not front-end hacks.

---

## 7. Quick checklist

- [ ] Get the app's origin and (for multi-app) confirm the Host routes to your app.
- [ ] `POST /auth/login` → keep the bearer token; send it on every call.
- [ ] Model your screens on `GET /api/<table>` (filter/select/order/embed/paginate).
- [ ] CRUD via `POST/PATCH/DELETE /api/<table>[/<id>]`.
- [ ] Anything non-CRUD → ask for an `rpc` hook; call `POST /rpc/<name>`.
- [ ] Web SPA → build into `public/`; native → hit the API directly.
- [ ] Generate a client from `GET /openapi.json` if you want types.
