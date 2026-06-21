# ClerkHalls — event-hall booking, on cellar

A real, growing app built on cellar: an **offline-first web SPA** for managing event-hall
bookings, backed by the sync API. Ported from the ClerkHalls Flutter app's data model;
we're filling it in iteratively.

**One bundle = one organization** (file-isolated). Inside it:

```
organization
 └─ venues                        (a location/building)
     └─ halls                     (bookable event spaces, ~30–500 cap)
         └─ bookings              (per hall + session + date)
             ├─ booking_item_categories
             ├─ booking_items     (menu / extra / wholesale / byo)
             └─ payments
menu_categories → menu_items      (catalog feeding booking_items of kind 'menu')
```

Every table is **syncable** (`rev` + `deleted`), so the whole org dataset syncs across
staff devices, offline-first, with `ON DELETE CASCADE` honored under sync.

## Run it

```sh
examples/clerkhalls/run.sh        # after building cellar (cmake --build build-cmake)
# open http://localhost:8080/  — sign in (prefilled)
```

Open a **second browser** as another device (separate `device_id`) to watch live sync.
Toggle **online → off** in the sidebar to queue edits offline; flip back on to push+pull.

## Architecture

Front-end is **Vue 3 + vue-router**, loaded as vendored global builds — **no build step**;
ES-module components served straight from `public/`. (Swap the `vendor/*.global.js` dev
builds for the `*.global.prod.js` ones in production.)

- `public/db.js` — the multi-table offline sync engine: a local mirror of every syncable
  table, a pending-mutation queue (`mutation_id` + `device_id`), `sync()` = push→pull,
  and realtime CHANGE applied live. Views read/write through it; it owns all network I/O.
  `db.state` is a Vue-reactive projection the views bind to.
- `public/store.js` — app-level reactive state + actions (auth flag, the generic modal,
  the sync lifecycle).
- `public/app.js` — root component: the shell (nav + sync bar + `<router-view>`), the
  router, and the auth gate.
- `public/views/*.js` — one component per screen (`auth`, `venues`, `bookings`, stubs).
- `public/components/*.js` — shared components (`syncbar`, the generic `modal`).
- `public/util.js` — shared helpers + domain enums (sessions, event types, statuses).
- `schema.sql` / `policies.json` / `hooks.lua` — the org bundle (data model, authz,
  server-set timestamps + a `resolve()` most-recent-edit-wins conflict rule).

## Status (iterative)

- [x] Foundation: bundle, multi-table sync engine, shell, auth, live/offline sync
      (Vue 3 + vue-router, build-less, component/view modules)
- [x] **Venues & Halls** — full CRUD (org → venue → hall), cascade delete
- [x] **Bookings** — calendar (per-venue, workflow coloring) + list, create/edit form,
      **offline conflict detection** (one live booking per hall+session+date)
- [x] **Booking detail** — itemized sections (Menu categorized · Wholesale · Extras) +
      payments + grand-total card (Σ items − discount − payments = balance due)
- [x] **Status lifecycle** — guided tentative→confirmed→completed→cancelled transitions
- [x] **Menu catalog** — `menu_categories`/`menu_items`, catalog-backed item picker
- [x] **Reports** — revenue (contracted/collected) + outstanding balances, by venue/month

## Sync-integrity test

`python3 test_sync.py` (against a running `run.sh`) exercises the offline-first sync
substrate end-to-end: cascade soft-delete, idempotent retry, LWW conflict resolution,
delete semantics, device cursors + delta pull, tombstone visibility, and batch
atomicity. Self-contained + re-runnable (unique per-run id prefix; cleans up after
itself). 22/22 checks; also cross-checked against cellar's `?aggregate=` endpoint.

Reset: delete `examples/clerkhalls/.run/` and the browser's localStorage.
