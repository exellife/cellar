# Tasklets — a cellar example app

A tiny **realtime collaborative task board** that demonstrates, end to end, what a
cellar backend is and how you write one. It fits in one bundle directory and uses
**every** layer of the engine:

- a **schema** (`schema.sql`) — declare tables; they become a REST API automatically;
- **authorization** (`policies.json`) — roles + per-row ownership, fail-closed;
- **server behavior** (`hooks.lua`) — the full hook contract: `before`, `authorize`,
  `after`, `rpc`, `on_realtime`;
- a **front-end** (`public/`) — a dependency-free SPA served by cellar itself;
- **realtime** — the browser subscribes over a WebSocket and the board updates live.

No build step, no framework, no external services. SQLite + Lua + static files.

## Run it

```sh
# from the repo root, after building cellar (cmake --build build-cmake):
examples/tasks/run.sh
# then open http://localhost:8080/
```

Sign in as the seeded admin (`admin@tasks.local` / `tasklets`) to see all tasks, or
**Create account** for your own private board. Open a second browser window and
watch changes stream between them — that's the realtime layer.

`run.sh [cellar-binary] [port]` lets you point at a different binary or port.

## What each piece teaches

### 1. `schema.sql` — your tables *are* the API
A table called `tasks` immediately becomes `GET/POST /api/tasks` and
`GET/PATCH/DELETE /api/tasks/<id>`. The catalog is introspected from SQLite at boot,
so there's no ORM and no migration step. Notice the schema does real work on its own
(**Layer 1**): `CHECK (status IN ('todo','doing','done'))` and
`CHECK (priority BETWEEN 1 AND 5)` are enforced by SQLite — a bad value comes back as
a `400` with **no hook code**. The `id` has a UUID `DEFAULT`, so clients never send one.

### 2. `policies.json` — who can do what
`_default: "deny"` makes the app fail-closed; only what's listed is allowed. Two
roles: `admin` (a `superuser`) and `member` (`self_register` — the UI's "Create
account"). Every `tasks` action is `owner_column: "owner_id"`, so the engine
transparently scopes each member's `list`/`get`/`update`/`delete` to *their own*
rows. No `WHERE owner_id = ...` in any client code — it's policy.

### 3. `hooks.lua` — server-side behavior, all five hooks
| hook | what it does here | why you can't do it client-side |
|---|---|---|
| `before(create)` | requires a title, forces `owner_id = who.user_id` | clients must not set ownership |
| `before(update)` | stamps `done_at` when a task is completed | derived server state |
| `authorize(create/update)` | **P5 "urgent" is admin-only** (a rule about a value) | beyond RBAC/ownership |
| `after(*)` | appends to an `activity` audit log (post-commit) | trusted side effect |
| `rpc("board_stats")` | per-column counts in one grouped query | custom, non-CRUD endpoint |
| `rpc("clear_done")` | bulk-archive completed tasks | custom mutation |
| `on_realtime` | a change is only streamed to its owner (admins see all) | live authz filter |

Hooks call `cellar.query(sql, params)` / `cellar.exec(sql, params)` (parameterized
SQL on this app's db) and `cellar.log.*`. They're sandboxed and **fail closed**: an
error in `before`/`authorize`/`on_realtime` denies the action.

### 4. `public/` — the front-end is just static files
cellar serves this app's `public/` for its Host, with an SPA fallback. The client is
three small modules:
- `cellar.js` — a ~120-line client: `login`/`register`, CRUD, `rpc`, and `subscribe`
  (the realtime WebSocket). Crib from it for your own apps.
- `app.js` — the board UI; every action is one call into `cellar.js`.
- `style.css` — no framework.

Open devtools → Network and you'll see plain `/auth`, `/api/tasks`, `/rpc/*` calls
and the WebSocket frames. That's the entire contract.

### 5. Realtime
`cellar.js`'s `subscribe("tasks", onChange)` opens a WebSocket and sends a small
binary `SUBSCRIBE` frame carrying the bearer token (the engine resolves identity
from it — no second login). Every allowed INSERT/UPDATE/DELETE arrives as a `CHANGE`
event and the board repaints. The `on_realtime` hook gates delivery per subscriber,
so a member only ever receives their own tasks.

## The bundle layout this produces

```
.run/localhost/            # CEL_APPS_DIR/<host>/  (host = localhost, port-stripped)
  data.db                  # identity schema (from provision) + your tasks/activity
  hooks.lua                # behavior
  policies.json            # authorization
  public/                  # the served front-end (index.html, app.js, cellar.js, style.css)
```

Delete `examples/tasks/.run/` to start over. To rehost the same app elsewhere, that
directory *is* the app — copy it (or `cellar export`/`import` it).

## Where to go next
- The client-author's guide: [`docs/frontend-guide.md`](../../docs/frontend-guide.md).
- The architecture + hook contract: [`docs/cellar-design.md`](../../docs/cellar-design.md) (§4 bundle, §7–9 hooks).
