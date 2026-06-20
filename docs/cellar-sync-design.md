# cellar — offline-first device sync (design note)

How an **offline-first client** (a Flutter app with its own local SQLite, but equally a
desktop or web app with IndexedDB/WASM-SQLite) keeps its data in step with a cellar app
across multiple devices — editing while offline, converging when reconnected.

Status: **design note, nothing built.** This is a forward-looking proposal that layers on
the existing engine; it does not change anything shipped. The main contract is
[`cellar-design.md`](cellar-design.md); this extends it. When code and this note disagree,
fix one on purpose.

Decision rule it inherits: sync is **policy** (it's about an app's data and merge rules),
so it lives in cellar — built from the primitives, with per-app behavior in `hooks.lua`.
portico stays the dumb transport.

---

## 1. The problem (and why it's five problems)

"Sync data between devices" is shorthand for five sub-problems. Any honest design answers
all of them:

1. **Change tracking** — what changed on each side since the last exchange.
2. **Delta transport** — `pull(changes since cursor)` + `push(local pending)`; never resend
   the whole dataset.
3. **Conflict resolution** — two devices edit the same row while both offline.
4. **Deletes** — a deleted row can't just vanish; the *deletion* must propagate, or it
   resurrects on the next pull. (Tombstones.)
5. **Row identity** — an offline `create` must mint an id that won't collide with another
   device's offline `create`.

Scope of v1: **row-level** sync of an app's tables, scoped by the same authz that governs
normal reads/writes. Not in scope: real-time collaborative *text* editing (character-level
merge), binary blob/file sync, or peer-to-peer sync without the server. (See §7–9.)

---

## 2. What cellar already gives us — and what's missing

Already there (the reason this is a thin layer, not a new subsystem):

| Need | cellar primitive |
|---|---|
| Collision-free offline creates (#5) | **UUID PKs** generated client-side and server-side alike |
| Live change stream while online (#1/#2) | the **realtime WS `CHANGE` feed** (INSERT/UPDATE/DELETE) |
| Per-write metadata + merge logic | **hooks** (`before`/`after`/`rpc`) run on every write |
| Only sync what a device may see | **per-row authz** (`owner_column`, `owner_via`) |
| One writer per app file → easy ordering | the **per-app write lock** (a natural place to assign a sequence) |

Missing today (the work this note scopes):

- The realtime feed is **live-only** — there's no "give me everything since sequence *X*",
  which is exactly what a device that was offline needs on reconnect.
- No **tombstone** convention (soft delete).
- No defined **push-with-conflict** protocol.
- No **per-device cursor** tracking (needed for safe tombstone GC and partial sync).

---

## 3. v1 model: server-authoritative delta sync (last-write-wins)

The pragmatic 90% design — the shape WatermelonDB / PouchDB-style apps use. The server is
the source of truth; devices reconcile against it. Chosen over CRDTs for v1 because it adds
**no new data-model runtime** and reuses the authz/hook path directly. CRDTs are the §7
upgrade for the cases LWW can't serve.

### 3.1 Data model — three columns on each *syncable* table

```sql
id       TEXT    PRIMARY KEY DEFAULT (<uuid4>),   -- already cellar's default
rev      INTEGER NOT NULL DEFAULT 0,              -- server-assigned, monotonic per app
deleted  INTEGER NOT NULL DEFAULT 0               -- tombstone (soft delete)
```

A table opts into sync by declaring these (or via a `policies.json` `"sync": true` flag that
the engine treats as "maintain rev/deleted for me"). `rev` is **not a wall-clock** — see 3.2.

### 3.2 The per-app change sequence (the cursor)

Every committed write stamps the row with the next value of a **per-app monotonic counter**.
Because cellar already serializes writes per app under the write lock, this is trivial and
race-free — a `before`/`after` hook, a trigger, or a dedicated counter row:

```
rev := app.next_rev()        -- atomic bump under the per-app write lock
```

Why a server sequence and **not** `updated_at` timestamps: device clocks skew, and LWW keyed
on a skewed clock silently keeps the *wrong* write. A server-assigned sequence gives a total
order with no clock trust. (If we later want offline-generated ordering without a round trip,
the upgrade is a **hybrid logical clock** — §6.)

Deletes are writes too: `DELETE` becomes `UPDATE … SET deleted=1, rev=next`. The row stays as
a tombstone until GC (§6).

### 3.3 Pull — `rpc("sync_pull", { since })`

```jsonc
// request
{ "since": 0, "tables": ["tasks", "activity"], "limit": 500 }
// response
{ "changes": { "tasks": [ {row…, "rev": 41, "deleted": 0}, … ] },
  "cursor": 57,            // new high-water mark to store locally
  "more": false }          // true → call again from the returned cursor (paging)
```

Server returns rows (including tombstones) with `rev > since`, **owner-scoped automatically**
— a device only ever pulls rows that user is allowed to see. The client upserts each into its
local SQLite (and applies `deleted=1` as a local delete), then persists `cursor`.

### 3.4 Push — `rpc("sync_push", { mutations })`

```jsonc
// request — each mutation carries the base rev the device last saw for that row
{ "mutations": [
    { "op": "put", "table": "tasks", "id": "…", "base_rev": 41, "values": { … } },
    { "op": "del", "table": "tasks", "id": "…", "base_rev": 41 }
] }
// response
{ "results": [ { "id": "…", "status": "applied", "rev": 58 },
               { "id": "…", "status": "conflict", "winner": "server", "row": { …rev 60 } } ],
  "cursor": 60 }
```

Per mutation, the server compares the row's **current `rev`** to the client's `base_rev`:
- `current == base_rev` → no concurrent change → apply, stamp a new `rev`, return `applied`.
- `current > base_rev` → **conflict** → resolve (3.5), return the canonical row so the client
  overwrites its local copy.

Crucially, push applies through the **same `authorize`/`before` hooks** as a normal write —
a device cannot push a row it couldn't otherwise create/update, and `before` still validates
and stamps server-owned fields. Sync is not an authz bypass.

### 3.5 Conflict resolution — LWW default, hook override

Default policy is **last-write-wins by server `rev`**: the incoming write wins (it's newer in
the total order) unless an app rule says otherwise. The override lives **server-side in
`hooks.lua`**, so every device agrees on the rule:

```lua
-- optional: resolve(table, incoming, current, who) -> 'incoming' | 'current' | merged-row
function resolve(tbl, incoming, current, who)
  if tbl == 'inventory' then
    incoming.qty = math.max(incoming.qty, current.qty)   -- domain rule, not blind LWW
    return incoming
  end
  return 'incoming'   -- default LWW
end
```

This is the design's payoff: the merge policy is **app logic in one place**, not duplicated
across clients. (LWW's cost: the loser's edit is dropped. Fine for tasks/notes; for counters
or collaborative text, that's the signal to reach for §7.)

### 3.6 Realtime is the *online* fast-path, not a second mechanism

While connected, a device subscribes to the WS `CHANGE` feed and applies events live — no
polling. On reconnect it does one `sync_pull(since=cursor)` to catch everything it missed
offline, then resumes the live feed. The feed and the cursor pull are **two faces of the same
change stream** (both ordered by `rev`); the cursor is what makes the live feed resumable.

---

## 4. Identity & scoping — devices vs. collaboration

Both fall out of the *same* mechanism; only the policy scope differs:

- **One user, many devices** (personal offline app): each device authenticates as that user;
  pull/push move that user's **owner-scoped** rows. The devices converge because they're all
  reconciling the same server rows.
- **Many users, shared data** (collaboration): rows visible via `owner_via`/membership sync to
  every member's devices. Same protocol — the policy just lets more rows through.

No special "device" entity is required for correctness. A `device_id` is still useful for
tombstone GC and for a device to ignore the echo of its own pushes (§6).

---

## 5. Worked example (a task created offline on phone A, edited on phone B)

1. **A offline**: `INSERT tasks(id=U, title='draft', rev=0)` locally; queue a `put` mutation.
2. **B online** already has the board; nothing yet for `U`.
3. **A reconnects** → `sync_push` `{put U, base_rev:0}` → server stamps `rev=58`, returns
   `applied`. The server CHANGE feed pushes `U@58` to B live; B inserts it.
4. **B edits** `U` title→'final', `base_rev:58` → push → server `rev=59`, `applied`.
5. **A**, still showing `rev=58`, pulls `since=58` → gets `U@59` → overwrites local. Converged.
6. **Concurrent case**: if A had also edited `U` at `base_rev:58` before pulling, its push
   arrives with `base_rev:58` while server is at `59` → **conflict** → LWW (or `resolve`)
   decides; A overwrites with the winner. No lost update *silently* — the client is told.

---

## 6. Hard parts (don't hand-wave these)

- **Tombstone GC.** Tombstones can't live forever, but you can only purge a deletion once
  *every* device has seen it — else a long-offline device resurrects the row. Requires
  tracking a **per-device cursor** (a `_sync_devices(device_id, user_id, cursor, seen_at)`
  table); GC purges tombstones with `rev < min(device cursor)`. A device offline past the GC
  horizon must do a **full re-sync** (treated like a fresh device).
- **Bootstrap / fresh device.** First sync is "everything since 0" — page it (`limit`+`more`),
  or seed from a snapshot (cellar's `export` already makes a consistent `data.db` copy).
- **Clocks.** v1 avoids them (server sequence). If offline ordering-without-round-trip is ever
  needed, upgrade `rev` to a **hybrid logical clock** (HLC) — still no wall-clock trust, but
  devices can order their own offline edits.
- **Security.** Push re-runs `authorize`/`before` per row (non-negotiable). Pull is
  owner-scoped by the existing policy. The sync RPCs are just structured CRUD — no new trust.
- **Idempotency / retries.** A push that times out after committing must be safe to retry —
  key mutations by `(id, base_rev)` (or a client mutation id) so a re-apply is a no-op.
- **Schema migration across versions.** A device on an older schema syncing to a migrated app
  — out of scope for v1; note it as a constraint (clients pin a schema version).

---

## 7. The alternative: CRDTs (when LWW isn't enough)

LWW silently drops the loser on concurrent edits. For counters, set-union, or collaborative
text, you want **conflict-free** merge. The notable option, because of the SQLite-on-both-ends
symmetry, is **cr-sqlite (vlcn)**: a SQLite extension that turns tables into CRDTs and
produces/merges compact **changesets**. Since cellar *is* SQLite and a Flutter client *can be*,
both ends could speak the same changeset format — true multi-writer convergence, even
peer-to-peer with cellar as a relay.

Trade-offs vs. §3: an **extension dependency** on both ends, per-row **causal metadata**
(bigger rows + payloads), and a merge model that's harder to reason about for app authors than
"LWW unless a hook says otherwise." Recommendation: **ship §3 first** (covers most apps), and
treat cr-sqlite as an opt-in per-table mode for the tables that genuinely need it.

## 8. Noted-and-not-chosen: SQLite session changesets

SQLite's built-in **session** extension records a changeset of mutations and can apply/rebase
with conflict handlers. Tempting (both ends are SQLite), but its conflict model is two-way
(client↔server), not designed for *N*-device convergence, and it needs the extension compiled
in on both ends. Useful as a wire format for the §3 push, perhaps; not a substitute for the
sequence/cursor/resolve protocol.

---

## 9. Non-goals (v1)

- Character-level collaborative text (that's a CRDT/OT problem — §7).
- File/blob sync (large binaries want content-addressed storage + range transfer, separate).
- Peer-to-peer / serverless sync (cellar-as-hub only in v1; cr-sqlite opens P2P later).
- Cross-app sync (a sync session is always within one app/`data.db`).

---

## 10. Build sequence (where it plugs in)

1. **Sequence + tombstones.** Per-app `rev` counter + the `before`/`after` hook (or trigger)
   that stamps `rev`/`deleted` on syncable tables. Decide the opt-in surface (`policies.json`
   `"sync": true` vs. explicit columns). *Touches:* `schema_catalog`, a hook/trigger, `app_db`.
2. **`sync_pull`.** A built-in RPC (or `GET /api/<table>?since=&include_deleted=1`) returning
   owner-scoped changes since a cursor, paged. *Touches:* `api.c`, query builder (a `rev >` +
   `include_deleted` mode).
3. **`sync_push`.** Batch apply with the rev compare + conflict path, through the existing
   `authorize`/`before` hooks; optional `resolve(...)` hook. *Touches:* `api.c run_write`
   (batch variant in one txn), `cel_hooks`.
4. **Per-device cursors + tombstone GC.** `_sync_devices` table, GC job. *Touches:* `app_db`,
   a maintenance CLI.
5. **Client guidance.** Extend [`frontend-guide.md`](frontend-guide.md) with the
   offline-first loop (local SQLite mirror, pull-on-reconnect, push queue, apply CHANGE live)
   and ship a reference implementation in `examples/` (a Flutter or web offline client).
6. *(later)* cr-sqlite opt-in mode for tables that need CRDT merge (§7).

---

## Open decisions

- **Opt-in surface:** `policies.json` flag vs. explicit `rev`/`deleted` columns vs. a `STRICT`
  per-table convention.
- **Transport:** dedicated `sync_pull`/`sync_push` RPCs (clean, versionable) vs. extending the
  CRUD query params (less new surface). Leaning RPC.
- **`rev` allocation:** a single per-app counter (simple, global order) vs. per-table counters
  (smaller cursors, more bookkeeping). Leaning single per-app.
- **Conflict default:** LWW-by-rev (proposed) vs. always-require a `resolve` hook for syncable
  tables (safer, more friction).
- **Where the merge runs for batch push:** one transaction per push (atomic, simpler rollback)
  vs. per-mutation (partial success). Leaning one txn, all-or-nothing, with per-row results.
