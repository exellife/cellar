# Offline Notes — a cellar offline-first sync demo

A notes app that **works offline** and syncs across devices through cellar's sync API.
It's the client side of [`docs/cellar-sync-design.md`](../../docs/cellar-sync-design.md):
a local mirror, a pending-mutation queue, push/pull, conflict resolution, and live
realtime — small enough to read in one sitting (`public/sync.js` is the whole client).

## Run it

```sh
examples/offline_notes/run.sh        # after building cellar (cmake --build build-cmake)
# then open http://localhost:8080/  — sign in (creds prefilled)
```

**Open two tabs.** Each tab is an independent **device** (its own local store + `device_id`,
kept in `sessionStorage`). Sign in as the same user in both.

## What to try

1. **Offline edits queue.** Toggle **online → off** in a tab. Add/edit notes — nothing hits
   the server (check the Network tab); the **N pending** badge climbs and unsynced notes get
   an orange edge.
2. **Going online syncs.** Toggle back **on** → the sync log shows `push …` then `pull …` and
   pending drops to 0. The other tab (if online) sees the changes **live** (realtime) or on its
   next sync.
3. **Conflict resolution.** Take both tabs offline. Edit the **same** note in each. Bring both
   online. The `resolve()` hook (most-recent edit wins) settles it — the log shows
   `conflict/server` or `conflict/incoming`, and both tabs converge to the same note.
4. **Retry safety.** Every queued write carries a `mutation_id`, so a flaky reconnect that
   re-sends the queue is a no-op (`(dup)` in the log) — no double-applies, no lost updates.

## How the client maps to the protocol (`public/sync.js`)

| client behavior | sync API |
|---|---|
| a note's `id` is a client-generated UUID | offline creates never collide → `put` with that id |
| each edit → a queued mutation `{op, id, base_rev, values, mutation_id}` | `POST /sync/push` body |
| `base_rev` = the last server-confirmed rev of the row | server detects conflicts |
| `sync()` = push the queue, then pull since the cursor | `/sync/push` → `/sync/pull` |
| `device_id` sent on every call | per-device GC cursor (`_sync_devices`) |
| `mutation_id` per write | idempotent retry (`_sync_applied`) — re-push is a no-op |
| apply pulled rows (incl tombstones) over local | server is authoritative |
| subscribe + apply `CHANGE` while online | the realtime WebSocket |

Server-side, `notes` is **syncable** (it declares `rev` + `deleted`), **owner-scoped** (each
member only syncs their own notes), and **realtime-enabled** — all in `schema.sql` +
`policies.json`. `hooks.lua` forces `owner_id` on create and implements the `resolve()` rule.

## The honest edges (v1)

- The **conflict loser sees the winning note via the next pull**, not inline in the push
  response (the demo pulls right after pushing, so it's seamless).
- `resolve()` picks a whole row (`'incoming'`/`'current'`); field-level merge is a planned
  follow-up.
- Tombstones are reclaimed out-of-band by `cellar sync-gc` (an operator/cron job), once every
  device has pulled past them.

## Reset
Delete `examples/offline_notes/.run/` (the provisioned bundle) and your browser's
sessionStorage for the tab. See also [`docs/frontend-guide.md`](../../docs/frontend-guide.md).
