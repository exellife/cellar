# NotifChannel — off-site notification delivery (design)

> **Status:** design of record (2026-07-01). Engine module E2.4. Phase-1 (this doc → code):
> the `NotifChannel` port + **email** adapter + engine-owned push **subscriptions** + the
> async **fan-out** (`cellar.notify`). Phase-2: the **Web-Push (VAPID)** crypto adapter.
> Companions: [`engine-modules.md`](engine-modules.md), [`policy-guide.md`](policy-guide.md).

## 1. Why

Classifieds notifications today reach a user **only while they're in the app**: `notify()`
(the bundle) inserts a `notification` row (the bell feed) + `cellar.rt_emit`s it over the open
WebSocket. Nothing reaches a user who's **away** — no browser push, email, or SMS. For a
marketplace ("new message", "saved-search match", "listing expiring") off-site delivery is the
point of E2.4.

## 2. The port

A generic engine port (same shape as `BlobStore`/`JobQueue`/`EventSink`): one interface to
deliver a message via some external channel, with pluggable adapters behind a name→adapter
registry. Adapters are **C** (network/crypto), not Lua.

```c
/* src/core/notif_channel.h */
typedef struct { const char *title, *body, *url, *data_json; } cel_notif_msg_t;

typedef struct {
    const char *name;                                  /* "email" | "webpush" | "sms" */
    /* recipient is channel-specific: an email address, or a subscription JSON. */
    int (*send)(const char *recipient, const cel_notif_msg_t *msg, char *err, int errlen);
} cel_notif_channel_t;

void cel_notif_register(const cel_notif_channel_t *ch);          /* at boot */
const cel_notif_channel_t *cel_notif_get(const char *name);     /* NULL = unknown */
```

**Adapters:**
- **`email`** — wraps `core/mailer.c` `cel_mail_send(to, subject, body)`. *(Phase 1.)* Note:
  real delivery is **ops-gated** (`cel_mail_enabled()` is false without SMTP/domain — same
  blocker as A1.8 magic-link), so it routes correctly but is a no-op until ops configures mail.
- **`webpush`** — VAPID: RFC 8291 payload encryption (P-256 ECDH → HKDF-SHA256 → AES128GCM) +
  RFC 8292 `Authorization` JWT (ES256), via OpenSSL (already linked); HTTP POST to the
  subscription `endpoint`. **Phase 2** (the crypto-heavy adapter).
- **`sms`** — optional, later.

## 3. Engine-owned subscriptions

Push subscriptions are generic auth-adjacent state (a credential tied to a `cel_users` row),
so they live in the **engine**, same class as `cel_sessions` / `cel_device_tokens` — reusable
by any bundle.

```sql
CREATE TABLE IF NOT EXISTS cel_push_subscriptions (
  id           TEXT PRIMARY KEY,                 -- public id
  user_id      TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
  endpoint     TEXT NOT NULL UNIQUE,             -- the push service URL (dedup key)
  p256dh       TEXT NOT NULL,                    -- client public key (base64url)
  auth         TEXT NOT NULL,                    -- client auth secret (base64url)
  ua           TEXT,                             -- optional user-agent label
  created_at   INTEGER NOT NULL DEFAULT (unixepoch()),
  last_used_at INTEGER,
  disabled_at  INTEGER                           -- set on a 404/410 from the push service (prune)
);
```
Applied via the versioned auth-schema path (base + an `AUTH_SCHEMA_V4` step for existing DBs —
the device-tokens migration is the template; **bump the version**, the table is engine-owned).

**Endpoints** (Bearer; mirror `/auth/device*`):
- `POST /push/subscribe   {endpoint, keys:{p256dh, auth}, ua?}` → `201 {id}` (upsert by endpoint)
- `POST /push/unsubscribe {endpoint | id}` → `200`
- `GET  /push/subscriptions` → the caller's own (no secrets beyond what they sent)

## 4. Async fan-out (`cellar.notify`)

The Lua primitive the bundle calls:

```
cellar.notify(user_id, { title=, body=, url=, data= })  -> true | nil,err
```

It does **not** send inline (network/SMTP must not block a hook). It **enqueues a JobQueue job**
with a **reserved type `cel:notif`** and a JSON payload `{user_id, msg}`. The job runner
(`cel_hooks_run_jobs`) gains a small branch: a `cel:notif`-typed job is handled **in engine C**
(resolve the user's channels → `cel_notif_get(ch)->send(...)`), instead of dispatching to the
bundle's Lua `job` hook. So fan-out is async and lives entirely in the engine — bundles never
implement it. Two properties matter here:

- **Never under the app write lock.** The worker holds `app_db_write_lock` across the job batch
  (so Lua handlers' writes serialize). Off-site delivery does **network I/O** and only **reads**
  the db (resolve recipients), so the `cel:notif` branch **drops the write lock around the send**
  and re-takes it — a slow/timing-out SMTP/push call can't stall request-thread writes.
- **Retried with backoff.** A transient channel failure (`send` < 0) marks the job failed → the
  JobQueue retries with backoff (→ dead-letter after max attempts), exactly like the Lua job
  path. A no-op (e.g. mail unconfigured) or "nothing to send" completes the job.

**Channel resolution** (engine): for `user_id` → the email channel (address from `cel_users`)
+ every active `cel_push_subscriptions` row. A `webpush` send that gets 404/410 sets
`disabled_at` (auto-prune). Per-channel failures are independent (one bad sub doesn't fail the rest).

**Bundle policy stays in the bundle.** classifieds' existing `notify()` keeps doing the in-app
feed + `rt_emit`, and *additionally* calls `cellar.notify(...)` — applying its own rules
(prefs, and "suppress off-site if the user saw it in-app within N minutes"). The engine owns the
*mechanism*; the bundle owns *when/who*.

## 5. Split of work

- **Engine (this work):** the port + adapters, `cel_push_subscriptions` + `/push*` endpoints,
  `cellar.notify` + the `cel:notif` fan-out in the job runner, VAPID config (Phase 2).
- **Bundle (classifieds backend):** call `cellar.notify` from `notify()`; the suppress/pref rule.
- **Frontend (other agent):** the service worker, push-permission prompt, and POSTing the
  subscription to `/push/subscribe`. (Documented as the client contract.)

## 6. Slice plan

- **Slice 1 (this build) — foundation, NO crypto:** the port + `email` adapter (+ a `test`
  capture adapter for e2e), `cel_push_subscriptions` + `/push*` endpoints, `cellar.notify` +
  the `cel:notif` fan-out job, classifieds `notify()` wired. Subscriptions are *stored*; a
  `webpush` send is a documented no-op until slice 2. Tests: port unit + e2e (subscribe →
  notify → channel invoked via the capture adapter) + the existing-DB migration.
- **Slice 2 — Web-Push (VAPID) adapter:** the RFC 8291/8292 crypto (OpenSSL) + HTTP POST +
  404/410 pruning + VAPID key config (`CEL_VAPID_PUBLIC/PRIVATE/SUBJECT`). Then off-site
  browser push actually delivers.
- **Later:** SMS adapter; a per-user preference model (which channels / which event types) if
  the bundle-side rules aren't enough.

## 7. Security / ops notes

- **VAPID keys** are deployment config (Phase 2); without them the `webpush` adapter is disabled
  (like `cel_mail_enabled()` for email) — fail-safe, not fail-broken.
- **Subscription hygiene:** dedup by `endpoint` (re-subscribe upserts); auto-disable on 404/410;
  `ON DELETE CASCADE` with the user; a user only sees/revokes their own.
- **No secrets leak:** `/push/subscriptions` returns the caller's own rows only; payloads are
  encrypted per-subscription (Phase 2), never logged.
- **Payload size:** Web-Push caps ~4 KB encrypted — keep `msg` small (title/body/url/short data).
