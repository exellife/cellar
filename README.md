# cellar

A multi-app backend engine in C. One process hosts many self-contained apps, each
an isolated **SQLite** bundle — its own data, behavior, front-end, and authorization
— routed by the Host header. Generates a REST/JSON + realtime API from each app's
schema, with a LuaJIT hook layer for custom logic. Installs on any Linux box as a
single service.

Design: **[docs/cellar-design.md](docs/cellar-design.md)**.
Building a client (web/Flutter/agent): **[docs/frontend-guide.md](docs/frontend-guide.md)**.

## The app bundle

An app is a directory:

```
apps/myapp/
  data.db        # schema + data (SQLite); your tables become /api/<table> endpoints
  hooks.lua      # optional server-side behavior (authorize/before/after/rpc/on_realtime)
  public/        # optional front-end (HTML/CSS/JS), served as a site (SPA fallback)
  policies.json  # optional per-app authorization
```

Isolation is the file boundary — no tenant ids, no shared tables. Provision an app
is "create the directory"; move/back-up/export it is "copy the file".

## Build

Requires: `gcc`, `cmake` (>= 3.16), and dev headers for `libsodium`, `uuid`,
`OpenSSL`, and `libcurl` (Ubuntu: `libsodium-dev uuid-dev libssl-dev
libcurl4-openssl-dev`). **SQLite, LuaJIT, and cJSON are vendored** under
`third_party/` (built from source) — no system packages needed. The
[portico](../portico) transport library is expected as a sibling checkout
(`../portico`) or a submodule at `external/portico`.

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j                 # -> build-cmake/cellar
```

No Postgres, no `-DSQLITE3_ROOT` — a fresh clone builds reproducibly.

## Tests

```sh
ctest --test-dir build-cmake --output-on-failure
```

C unit tests plus end-to-end suites that each boot a server on a free port (need
`python3` + the `websockets` module). No external database.

## Run

### Single-app (the simple deployment)

```sh
CEL_DATA_DB=./myapp.db CEL_SEED_ADMIN="admin@example.com:s3cret-pw" ./build-cmake/cellar
```

Every request hits the one database; `http://localhost:8080/` serves the embedded
admin UI.

### Multi-app (many apps, routed by Host)

```sh
# scaffold a bundle (data.db + hooks.lua + public/ + a seeded admin)
CEL_APPS_DIR=./apps ./build-cmake/cellar provision shop.example admin@shop.example 's3cret-pw'

# serve every bundle under ./apps, routed by the Host header
CEL_APPS_DIR=./apps ./build-cmake/cellar
```

A request with `Host: shop.example` resolves to `./apps/shop.example/`.

### Configuration (env vars)

| Var | Default | Purpose |
|---|---|---|
| `CEL_PORT` | `8080` | listen port |
| `CEL_LOG_LEVEL` | `info` | debug/info/warn/error |
| `CEL_DATA_DB` | `cellar.db` | single-app database file |
| `CEL_APPS_DIR` | — | multi-app: bundles under `<dir>/<host>/` (enables Host routing) |
| `CEL_CONTROL_DB` | — | optional control-plane registry (gates routing; enables suspend/resume) |
| `CEL_POLICY_FILE` | — | default/single-app authorization config (per-app `policies.json` overrides it) |
| `CEL_SEED_ADMIN` / `CEL_SEED_USERS` | — | first-run seeding (`email:password` / `email:pass:role;…`) |

## CLIs

```sh
cellar provision <host> [admin-email admin-pw]   # scaffold + register a new app bundle
cellar export <host> <out.tar.gz>                # package an app (consistent live snapshot)
cellar import <host> <in.tar.gz>                 # reconstitute an app (rehost by new <host>)
cellar apps                                      # list registered apps + status   (needs CEL_CONTROL_DB)
cellar suspend|resume <host>                     # take an app offline/online (live) (needs CEL_CONTROL_DB)
cellar revoke-sessions <email> | mfa-reset <email> | unlock <email> | send-test-mail <to>
cellar passwd <email> <new-password>             # out-of-band password reset (no email round-trip)
```

`export`/`import`/`suspend`/`resume` operate against a running server safely.

## API

REST/JSON with bearer-token auth, plus an optional realtime WebSocket. The full
contract is in **[docs/frontend-guide.md](docs/frontend-guide.md)**:

- `POST /auth/login` → `{token, user}`; send `Authorization: Bearer <token>`.
- `GET/POST/PATCH/DELETE /api/<table>[/<id>]` — CRUD with filter / select / order /
  embed / keyset-pagination / aggregates.
- `POST /rpc/<name>` — custom endpoints defined in the app's `hooks.lua`.
- `GET /schema`, `GET /openapi.json` — introspection + client codegen.

## Deploy (systemd)

The binary is self-contained (SQLite, LuaJIT, cJSON, web assets all built in / embedded).

```sh
sudo cmake --install build-cmake --prefix /usr/local        # -> /usr/local/bin/cellar
sudo cp /usr/local/share/cellar/cellar.service /etc/systemd/system/
sudo cp /usr/local/share/cellar/cellar.env.example /etc/cellar/cellar.env   # then edit
sudo systemctl daemon-reload && sudo systemctl enable --now cellar
```

Runs as a locked-down service user; logs to journald (`journalctl -u cellar`).

## Realtime wire protocol (optional)

Binary frames over WebSocket, 8-byte big-endian header + JSON payload:

```
[opcode:1][flags:1][message_id:2][payload_length:4][payload:N]
```

Used for live `CHANGE` events (subscribe to a table, receive pushes). Pure-REST
clients ignore it entirely.
