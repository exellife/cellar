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

---

## Resolved

_Engine fixes that came out of dogfooding (the loop working). New resolutions go on top._

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
