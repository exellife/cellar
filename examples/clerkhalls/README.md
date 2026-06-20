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

- `public/db.js` — the multi-table offline sync engine: a local mirror of every syncable
  table, a pending-mutation queue (`mutation_id` + `device_id`), `sync()` = push→pull,
  and realtime CHANGE applied live. Views read/write through it; it owns all network I/O.
- `public/app.js` — shell: auth, nav, hash router, sync status bar.
- `public/views/*.js` — one module per screen.
- `schema.sql` / `policies.json` / `hooks.lua` — the org bundle (data model, authz,
  server-set timestamps + a `resolve()` most-recent-edit-wins conflict rule).

## Status (iterative)

- [x] Foundation: bundle, multi-table sync engine, shell, auth, live/offline sync
- [x] **Venues & Halls** — full CRUD (org → venue → hall), cascade delete
- [x] **Bookings** — list + create/edit form (hall · session · date · event · guests · price ·
      status), with **offline conflict detection** (one live booking per hall+session+date)
- [ ] Booking detail (itemized lines + payments + grand total)
- [ ] Menu catalog
- [ ] Reports (revenue, outstanding balances)

Reset: delete `examples/clerkhalls/.run/` and the browser's localStorage.
