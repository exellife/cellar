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

### [OPEN] Gate listing-create + contact-reveal on email_verified (item 7 of the verification ask)
- **kind:** gap (+ product decision)
- **severity:** medium — the **6-digit code flow (items 1–6) is SHIPPED** (`45ed373`, see Resolved).
  What remains is item 7: **require `email_verified` before a user can post a listing or reveal a
  seller's contact** (browsing stays fully open). Distinct from the code flow, and a live-behavior
  change on a public site — so it needs the human's product call before it goes on.
- **what's needed (when approved):**
  - Reject listing-create and the contact `reveal_contact` rpc for an **unverified** user with a
    **stable, distinguishable** error — proposal: `403` + `{ error:"email_not_verified" }` — so the
    client opens the verify prompt instead of showing a generic failure. Bundle-side gate in
    `apps/classifieds/hooks.lua` (`before(create, listings)` + `reveal_contact`); the engine already
    carries `email_verified` on the session, exposed to hooks' `who` (or the hook reads `cel_users`).
- **open product question (for the human, not the engine):** turning this on adds a verify step to the
  post/contact funnel on the LIVE site (SES is on, so it works) — friction vs. trust. Options: (a) hard
  gate now; (b) gate only contact-reveal (higher-abuse) but leave posting open; (c) keep soft (trust
  tiers already scale limits by `email_verified`) and defer the hard gate. **Not enabling unilaterally.**
- **filed:** 2026-07-04 by frontend-agent (classifieds/Jarchy); items 1–6 resolved 2026-07-04 by cellar-agent.

---

## Resolved

_Engine fixes that came out of dogfooding (the loop working). New resolutions go on top._

- **Email verification — 6-digit code (soft, non-blocking)** — SHIPPED `45ed373`. Answers to the two
  questions first: **(a)** SES **is** enabled for classifieds (jarchy inherits process-wide
  `no-reply@svngn.com` / SES:2465, no `_mail` override). **(b)** The engine already had verification
  (`email_verified` on the user, `/auth/verify-email` + `.../resend`) but as a **magic-link token** — so
  it was rebuilt as the requested **6-digit code**. Contract (frontend items 1–6 all met):
  - **item 1** `user.email_verified` — already on the register/login user object. ✓
  - **items 2–3** register auto-sends a code; **`POST /auth/verify-email { code }`** is now **Bearer-
    authenticated** (the session identifies the user): `200` verifies, `400` on wrong/expired. Code is
    **6-digit, sha256 at rest, 15-min TTL, single-use, 5-attempt cap** (then dead → resend). ✓
  - **item 4** **`POST /auth/verify-email/resend`** (Bearer) → `200`; a **server-side ~60s cooldown** is
    enforced in the engine (`CEL_AUTH_LOCKED` → no send), not just the client timer. ✓
  - **item 5** **RU** subject/text/html — jarchy's `render_email` hook renders the code in Russian
    (`ctx.code`); other apps get the engine's plain default. ✓
  - **item 6** already-verified / OAuth (pre-verified) → no code minted (`CEL_AUTH_CONFLICT`). ✓
  - Engine internals: `cel_random_code` (bias-free), `cel_email_verifications` re-keyed (v4→v5 migration),
    `cel_auth_create_email_code` / `cel_auth_verify_email_code`. Tests: `auth_sqlite_test.c` unit cases +
    the `email_verification` e2e ported to the code flow; **81/81** green. Contract in
    `docs/frontend-guide.md`. **Endpoint names:** kept the engine's `/auth/verify-email` (+ `/resend`)
    rather than the proposed `/auth/email/*` — same behavior, wire to these. (**item 7 — the hard gate —
    is a separate Open item pending a product decision.**)

- **classifieds notifications — "mark read" already exists (contract mismatch)** — WORKING AS INTENDED,
  no engine/bundle change. The badge can be cleared; the client was calling the wrong endpoints
  (`mark_notifications_read` isn't whitelisted → 403; `PATCH /api/notification` is admin-only by design →
  403). Everything the bell needs already exists — full contract (all owner-scoped, role `user`):
  - `POST /rpc/notifications {unread_only?}` → `{ notifications:[{id,type,title,body,subject_id,read_at,created_at}] }`
  - `POST /rpc/unread_count` → `{ unread:N }`
  - **`POST /rpc/mark_read {id?}`** → `{ unread:N }` — omit `id` = mark ALL of mine read; pass `id` = that one. ← the badge-clear.
  - realtime: `SUBSCRIBE {table:"notification"}` → live `CHANGE` on new ones (owner-scoped).
  - **types + `subject_id` (for deep-link routing):** `message` → `subject_id` = conversation_id;
    `saved_search` → subject_id = matching listing id; `listing_expired` → subject_id = listing id.
  Documented in `docs/frontend-guide.md`. (frontend: swap `mark_notifications_read` → `mark_read`, and
  read via the `notifications` rpc rather than `/api/notification`.)

- **dev test logins (non-admin) + `cellar passwd` CLI** — the classifieds `run.sh` now provisions two
  stable `role=user` accounts out of the box (idempotent, dev only), so the frontend has non-admin creds:
  - `admin@classifieds.local` / `classifieds01` — role `admin`
  - `seller@classifieds.local` / `test1234` — role `user` (the seeded `--owner`: **owns 45 listings**, so
    `my_listings` returns real rows)
  - `buyer@classifieds.local` / `test1234` — role `user` (for favorites / reveal / chat flows)

  These survive a fresh `.run/` re-provision. To reset any password out-of-band (no email round-trip),
  the engine gained a CLI verb: **`cellar passwd <email> <new-password>`** (mirrors the `cellar.set_password`
  primitive — sets the hash, revokes the user's sessions + pending MFA + trusted devices, clears lockout).
  `CEL_DATA_DB=<bundle>/data.db cellar passwd <email> <pw>`. e2e in `auth_hardening_test.sh`.

- **classifieds `search` — category is now subtree-inclusive** — `search {category}` matches the given
  node **and all its descendants**, so browsing a parent (e.g. `cat-transport`) returns the whole
  subtree (`cat-cars` + `cat-moto`), while a leaf still returns just itself. Resolved in the bundle
  (`apps/classifieds/hooks.lua`) — the engine owns the tree, so a one-shot recursive CTE expands
  id-or-slug → `{self + descendants}` and the query filters `category_id IN (…)`. The client no longer
  needs to fetch the tree and OR ids. Sidebar **facets stay leaf-only** (a parent is heterogeneous → no
  filterable attrs → no facet block), exactly as noted; per-row `highlights` still resolve from each
  listing's own leaf category. Unknown category still → 0. e2e: `test_phase0.py` (parent ⊇ leaf,
  parent-has-no-direct-listings, unknown→0). Verified live on the seeded bundle: `cat-transport` 0 → 90.

- **classifieds `search` rows — display enrichments** — the `search` rpc rows now also carry
  `condition`, `price_negotiable`, `district_id`, `seller_id`, `seller_name`, `saved_count`, and
  `highlights` (a short category-composed specs array from `listing_facet`, e.g. `["1995","212 372 км",
  "Автомат"]`; unit-bearing numbers get a thousands separator, unitless like year stay plain). All
  additive. `seller_name` comes from the new `user_profile` table (below), null until the seller sets one.
- **classifieds `listing` — public seller block** — `listing` now returns
  `seller:{ id, name, member_since (unix epoch), listing_count (active), verified }` alongside
  `{listing, favorite_count, favorited}`. Phone stays OUT (reveal-only). `name` from `user_profile`.
  NEW: `user_profile(id=cel_users.id, display_name)` app table + a `set_profile {name}` rpc (auth, own)
  so users set a public display name (the engine's `cel_users` has none). `display_name` is null until set.
- **classifieds media — square `thumb`** — `thumb` is now a **400×400 center-crop** (was a 320 fit-box),
  so cards are uniform 1:1 without client cropping. Added `image_encode_jpeg_square` (center-crop cover,
  never upscales) in the engine image lib; `media.c` `thumb` variant uses it. `full` unchanged. Covered by
  `image_proc` + `media` tests. NOTE: dev-seeded thumbs predate this — re-seed or re-upload for square dev thumbs.
- **classifieds `my-listings` — engagement stats** — a new `my_listings` rpc (auth, owner-scoped) returns
  the caller's listings each with `views_count` (from the `listing_viewed` EventSink events) and
  `contacts_count` (from `contact_event`). `/api/listings` (generic CRUD) can't compute these, so this is
  the rpc to use for the seller's management page.

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
