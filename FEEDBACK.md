# Engine feedback — frontend → cellar

The channel for the **frontend** effort (a separate workspace, e.g. the ClerkHalls SPA)
to report what it needs from the **cellar** engine. cellar is the product; the frontend
is a consumer of cellar's contract (REST + `/sync` + WS, and the bundle format:
`schema.sql` / `policies.json` / `hooks.lua`). When the frontend hits a bug, a missing
capability, an ergonomic rough edge, a doc gap, or a confusing behavior, **file it here
instead of working around it silently** — engine problems get fixed in the engine.

## Workflow

- **Frontend agent / dev** → *writes* entries under **Open** (append; don't edit others').
  Don't change cellar's C code — describe the need and a repro; let the engine owner fix it.
- **Cellar agent (engine owner)** → *triages*, fixes in `src/` with a ctest, then moves the
  entry to **Resolved** with the commit hash + a one-line resolution. If it's working as
  intended, move it to Resolved with an explanation (or to **Won't fix** with the reason).
- Keep `docs/frontend-guide.md` + `docs/policy-guide.md` as the published contract — if a
  fix changes behavior, the engine owner updates those too.

## How to file (copy this template)

```
### [OPEN] <short title>
- **kind:** bug | gap | ergonomics | docs | question
- **severity:** blocker | high | medium | low
- **what I was doing:** <the feature / call you were building>
- **expected:** <what you expected cellar to do>
- **actual:** <what it did — include status code + message>
- **repro:** <curl / request body + response, or the exact API call>
- **filed:** YYYY-MM-DD by <who>
```

`kind`: **bug** = wrong/broken behavior · **gap** = capability that doesn't exist ·
**ergonomics** = works but awkward/verbose · **docs** = contract unclear/wrong ·
**question** = "is X possible / intended?"

---

## Open

### [OPEN] `/sync/push` dedup replay omits the `winner` field
- **kind:** ergonomics
- **severity:** low
- **what I was doing:** reconciling conflict outcomes from a push response without a follow-up pull.
- **expected:** a deduped retry of a conflicting mutation returns the same shape as the first
  response, including `winner` (`"incoming"` / `"server"`).
- **actual:** the idempotent-retry path returns `{status, rev, deduped:true}` but no `winner`,
  so a retried conflict loses the who-won signal. (A pull reconciles regardless, so it's minor.)
- **repro:** push a mutation that resolves as a conflict, then push it again with the same
  `mutation_id` → second response has no `winner`.
- **filed:** 2026-06-22 by cellar-agent (seed example — known backlog item)

### [OPEN] classifieds `search` rows — add display enrichments for the result cards
- **kind:** gap
- **severity:** medium
- **what I was doing:** building the classifieds web client's search/browse cards (the row list + grid) against a mock, ready to wire to the live `search` rpc.
- **expected:** each result row carries enough to render the card the design shows (condition badge, seller line, negotiable/save-count, a short specs preview).
- **actual:** the row is lean — `id, title, price, currency, category_id, city_id, photos, created_at` (+ `status` on favorites/my-listings). Without more, cards can only show title / price / city / time. (The UI degrades gracefully — these are additive.)
- **needed (all optional on the row):** `condition`, `price_negotiable`, `seller_id` + `seller_name`, `district_id`, `saved_count` (= the `favorite_count` you already compute for the detail rpc), `highlights` (a short category-composed specs array, e.g. `["2019","78 000 км","Автомат"]` for cars, `["1 комн","42 м²","5/9 эт"]` for apartments).
- **filed:** 2026-07-01 by frontend-agent (classifieds web client)

### [OPEN] classifieds `listing` — add a public seller block to the detail response
- **kind:** gap
- **severity:** medium
- **what I was doing:** the listing detail page's seller card (avatar/name/verified/"на сайте с …/N объявл.").
- **expected:** the `listing` rpc returns public seller identity alongside the listing.
- **actual:** it returns `{ listing, favorite_count, favorited }` — no seller info, so the seller card can't render name/verified/member-since/count.
- **needed (optional):** `seller: { id, name, member_since, listing_count, verified }`. The phone stays OUT of this payload — it's reveal-only via `reveal_contact`.
- **filed:** 2026-07-01 by frontend-agent (classifieds web client)

### [OPEN] classifieds media — a center-cropped square `thumb` variant
- **kind:** ergonomics
- **severity:** low
- **what I was doing:** card thumbnails render at 1:1 (phone photos are mixed portrait/landscape; a uniform square grid is the target).
- **expected:** a square `thumb` variant so cards stay crisp/consistent without client-side cropping tricks.
- **actual:** the `thumb` variant's aspect isn't square; the client forces 1:1 with `object-cover`, which crops unpredictably and can waste bytes.
- **needed:** a square `thumb` variant (e.g. 400×400, center-cropped) served at `/media/<id>/thumb`.
- **filed:** 2026-07-01 by frontend-agent (classifieds web client)

### [OPEN] classifieds `my-listings` — optional per-listing engagement stats
- **kind:** gap
- **severity:** low
- **what I was doing:** the seller's "Мои объявления" management page.
- **expected:** each owned listing can show simple performance (views, contacts) like most marketplaces.
- **actual:** the owner-scoped `listings` read returns the row without any stats.
- **needed (optional):** `views_count` / `contacts_count` (or a `stats` sub-object) on the owner-scoped listing read.
- **filed:** 2026-07-01 by frontend-agent (classifieds web client)

---

## Resolved

_Engine fixes that came out of dogfooding (the loop working). New resolutions go on top._

- **classifieds chat — realtime message delivery over WS** — WORKING AS INTENDED, no engine change.
  `message` is `realtime:true` + `owner_via` (conversation_member), so the engine already pushes a
  live `CHANGE` for each new message to the two participants and denies non-members' subscribes —
  proven by the `classifieds_chat_ws` e2e. The gap was docs: the WS subscribe shape wasn't written
  down, so the client polled. Documented the binary frame protocol + the concrete chat/notification
  subscriptions in `docs/frontend-guide.md §5` (subscribe `{table:"message", key:{column:"conversation_id",
  value:<id>}}` → `CHANGE`; notifications = `{table:"notification"}`, owner-scoped). Also noted the
  dev-proxy needs `ws:true` (WS is Host-routed like the API). Drop the polling; no backend work.

- **No revocable device token for PIN "fast sign-in"** — added device tokens: a long-lived, revocable
  credential a client stores (e.g. encrypted behind a PIN) and exchanges for fresh sessions without
  re-entering the password. `POST /auth/device {label?}` (Bearer) → `{id, device_token}` (shown once);
  `POST /auth/session/from-device {device_token}` → `{token, user}` (public, rate-limited, no MFA step —
  the enrolled device is the possession factor); `GET /auth/devices` + `POST /auth/devices/revoke {id}`
  (own, Bearer). Token hashed at rest; **opt-in per app** via `_session.device_ttl_seconds` (0/absent →
  endpoints 404). **Static/reusable** (rotation can layer on later). Account recovery (password reset /
  `set_password` / log-out-everywhere) revokes all the user's device tokens — which is also the admin
  force-revoke. `1cc0e20` (e2e in `device_token_test.py` + unit in `auth_sqlite_test.c`; client contract
  in `docs/frontend-guide.md`).

- **No way to reset an EXISTING user's password without email (admin set-password)** — added the
  `cellar.set_password(email, new_password) → true, err` Lua primitive, the update-side sibling of
  `cellar.create_user`: sets the password on an existing `password` identity with NO current-password
  check (keyed by email) and revokes the target's sessions + pending MFA + clears lockout. The user then
  sets their own via the authenticated `POST /auth/password/change`. **No separate `user_role` primitive
  needed** — a hook can already read the target's role via `cellar.query('SELECT role FROM cel_users …')`,
  so the bundle self-serves who-may-reset-whom. Safety floor: the engine refuses resetting a **superuser**
  (admin-takeover guard, mirroring create_user's refuse-to-mint-superuser); rpc-only (write-lock-reentry
  guard, no deadlock). `13631b5` (e2e in `rpc_test.py` + `create_user_ctx_test.py`; example in
  `docs/frontend-guide.md`). Suite 76/76, ASan-clean.
- **No authenticated in-session "change password" endpoint** — added
  `POST /auth/password/change {current_password, new_password}` (Bearer): verifies the
  current password (constant-time) then sets the new one; no email round-trip, the session
  stays valid; `401` on wrong current. `fee1617` (e2e in `password_reset_test.py`).
- **No way for a non-superuser role to create a login account** — added the
  `cellar.create_user(email, password, role) → id, err` Lua primitive, so a bundle `rpc`
  can mint logins under its own authz (e.g. a `manager` creates `clerk`s only) and capture
  the new id for a roster row. `POST /auth/users` stays superuser-only; the engine refuses
  `platform_admin`, the bundle enforces who-may-create-whom. `fee1617` (e2e in `rpc_test.py`;
  example in `docs/frontend-guide.md`).
- **Cascade soft-delete didn't emit realtime** — a parent delete tombstoned children but
  subscribers to child tables only saw it on the next pull. Now fires a live `DELETE` per
  cascaded child. `016737a` (+ rev-waste follow-up `ef196c0`).
- **`realtime`-only policy entry silently denied all CRUD** — adding `{ "realtime": true }`
  to a table fail-closed its CRUD for non-superusers. A metadata-only entry now falls through
  to `_default`. `a06d964`.
- **Malformed `where`-tree returned 500** — `{"not":{}}` / `{"and":[{}]}` emitted bad SQL.
  Now a clean 400, plus a nesting-depth cap. `1d28d2b`.

## Won't fix / by design

- **`del` with a stale `base_rev` returns `status:"conflict"` but still deletes** — this is
  correct LWW: read `winner` (`"incoming"` ⇒ your delete won). Not a bug.
