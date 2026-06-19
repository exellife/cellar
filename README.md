# cellar

A Supabase-style, schema-driven backend (PostgreSQL + C). Generates an API and admin
UI from your database schema. Installs on any Linux box as a single service.

See **[PLAN.md](PLAN.md)** for the full design and roadmap.

Status: **Phase 0 complete** — platform boots; `PING`/`ECHO`/`SERVER_INFO` and the
DB-backed auth flow (`LOGIN`/`VERIFY_SESSION`/`LOGOUT`) work end-to-end.

## Build

Requires: `gcc`, `cmake` (>= 3.16), and dev headers for `libpq`, `libsodium`,
`libcjson`, `uuid` (Ubuntu: `postgresql-server-dev-all libsodium-dev libcjson-dev
uuid-dev`).

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j          # -> build-cmake/cellar
```

Vendored libraries live under `lib/` (wslib, opcode_dispatcher, logger) and are built
as static libs by the same configure step.

A plain `make` (using the top-level `Makefile` -> `build/cellar`) is also available
as a no-CMake fallback.

## Tests (CTest)

```sh
ctest --test-dir build-cmake --output-on-failure
```

Each test boots its own server on a free port (via `tests/run_with_server.py`) and
needs python3 + the `websockets` module and a reachable Postgres. Tests:
- `smoke` — protocol + auth + schema (`scripts/smoke_test.py`)
- `wslib_adversarial` — WebSocket framing/robustness probes (`tests/wslib_test.py`)

## Database

Create an empty database, then let cellar bootstrap it — the migrations are
embedded in the binary and applied in order, idempotently:

```sh
createdb cellar                      # or: psql -U postgres -c 'CREATE DATABASE cellar'
./build-cmake/cellar migrate         # apply core migrations  (migrate status to inspect)
./build-cmake/cellar migrate --demo  # also load the demo tables (products/categories/notes)
```

Migrations are embedded in the binary, applied in order under an advisory lock,
each transactional and recorded in `cel_migrations` with a checksum (so an applied
migration can't be silently edited). Set `CEL_AUTO_MIGRATE=1` to apply core
migrations automatically on startup.

## Run

Configuration is via environment variables:

| Var | Default | Purpose |
|---|---|---|
| `CEL_PORT` | `8080` | WebSocket listen port |
| `CEL_LOG_LEVEL` | `info` | debug/info/warn/error |
| `CEL_DB_HOST`/`PORT`/`NAME`/`USER`/`PASSWORD` | localhost/5432/cellar/postgres/— | Postgres connection (empty password falls back to `~/.pgpass`) |
| `CEL_DB_POOL` | `8` | connection pool size |
| `CEL_SEED_ADMIN` | — | first-run admin upsert, `email:password` |
| `CEL_SEED_USERS` | — | multi-user seed, `email:pass:role;...` (roles: admin/editor/viewer) |
| `CEL_POLICY_FILE` | — | optional authorization overrides (row-level ownership) |

```sh
CEL_SEED_ADMIN="admin@cellar.dev:s3cret-admin" ./build-cmake/cellar
```

Then open **http://localhost:8080/** for the embedded **admin UI** (a petite-vue SPA
served from inside the binary) and sign in. It renders a schema-driven dashboard —
sidebar of tables, data grid, create/edit/delete — with zero per-table code.

## Deploy (systemd)

The binary is self-contained except for `libpq` — the web assets, migrations, and
admin UI are all embedded. A `-DCELLAR_STATIC=ON` build additionally statically links
libsodium/libuuid/cJSON, leaving `libpq5` (+ glibc) as the only runtime dependency.

```sh
cmake --build build-cmake -j
sudo cmake --install build-cmake --prefix /usr/local       # -> /usr/local/bin/cellar
sudo useradd --system --no-create-home --shell /usr/sbin/nologin cellar
sudo mkdir -p /etc/cellar
sudo cp /usr/local/share/cellar/cellar.env.example /etc/cellar/cellar.env
sudo chmod 600 /etc/cellar/cellar.env                    # then edit DB settings
sudo cp /usr/local/share/cellar/cellar.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now cellar
```

The unit runs `cellar migrate` before start (auto-bootstraps the schema) and runs as a
locked-down service user. Logs go to journald (`journalctl -u cellar`).

## Smoke test

```sh
python3 scripts/smoke_test.py            # needs the `websockets` pip module
# uses CEL_TEST_EMAIL / CEL_TEST_PASSWORD (defaults match the seed example above)
```

## Wire protocol

Binary frames over WebSocket, 8-byte big-endian header + payload:

```
[opcode:1][flags:1][message_id:2][payload_length:4][payload:N]
```

Opcodes are defined in `src/core/protocol.h`. Data-layer ops (`DB_*`, 0xD0–0xD7) are
generic and schema-driven — the target table is named in the JSON payload, not encoded
as a distinct opcode. (Engine lands in later phases.)
