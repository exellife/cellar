# cellar engine modules & ports

> **Intent:** the new engine primitives cellar needs (first surfaced by the classifieds design —
> see [`classifieds-design.md`](classifieds-design.md) §5 + roadmap), framed as **independent,
> swappable modules behind interfaces**. This is reusable engine work, not classifieds-specific.

## Core vs addon boundary

> Extends VISION's **mechanism-vs-policy** rule one level up: portico = transport; **cellar = generic
> primitives**; the **app bundle = domain**. cellar must stay generic — it never learns the word "listing".

**The rule.** *Engine* = anything domain-agnostic an **unrelated** app (a CRM, a recipe site) would
plausibly want, that never names a domain noun. *Addon* = anything encoding the classifieds domain
(its schema, rules, policies, UI). Litmus: *"delete the classifieds app — does this still make sense
in cellar?"* Yes → core; only-for-classifieds → addon.

**Two senses of "addon":**
1. **Generic engine modules** — domain-agnostic primitives not every deployment needs (storage,
   search, jobs, captcha, push, passkeys, webhooks). **Part of cellar**, but *optional + pluggable
   behind ports* (below) — keeps the core lean while staying generic.
2. **The domain bundle** — the classifieds itself (`schema.sql` + Lua hooks + policies + assets).
   **Not cellar** — an app built on it.

So classifieds requirements = generic primitives (engine) **composed by** a bundle (hooks/schema/
policies). Almost nothing classifieds-specific enters cellar.

**Where each thing lands** — the right column is all *domain nouns*; the left names none. *That* is the line:

| Capability | Engine core / module (generic) | Classifieds bundle (domain) |
|---|---|---|
| Storage | `BlobStore` | which blob is a listing photo |
| Image | resize / encode / pHash lib | photo rules, NSFW policy |
| Search | FTS + faceted-query over *arbitrary* schema | the facets (make/year), ranking weights |
| Jobs | `JobQueue` | expiry / payout / alert jobs |
| Events | `EventSink` | which events, the rec logic |
| Captcha | `CaptchaVerifier` | *when* to require it |
| Notifications | `NotifChannel` (email/push/sms) | message / expiry templates |
| Auth | passkeys / OAuth / TOTP / sessions | who may post, seller roles |
| Webhooks | verify + `IdempotencyStore` | payment logic, ledger |
| Sandbox + bundles + multi-app | the *mechanism* | the pro-storefront *product* |
| Rate limit | generic limiter | velocity thresholds |
| Recommendations | events + query + jobs primitives | the ranking / precompute algorithm |
| Trust & safety | rate-limit, captcha, pHash, events, generic status/flag fields | blocklists, risk weights, moderation queue, enforcement |
| Data / CRUD / policy | generic | listings/categories schema + access policies |

**Two anti-patterns to guard against:**
- **Domain leaking *into* core** — a hardcoded `listings` table, a "moderation engine" or "feed
  ranking" with classifieds assumptions. ❌ The engine gives flags/events/query/jobs; the *rules*
  are the bundle's.
- **Reinventing primitives *in* the bundle** — storage / queues / search written in Lua hooks because
  the engine lacks the primitive. ❌ The SQLite-coupling mistake at the app layer — add it to the
  engine (behind a port) and call it from the hook.

## Principle: port-first (the lesson from SQLite)

The SQLite mistake wasn't *using* SQLite — it was letting **storage assumptions leak through the
codebase** (no clean port), so the engine can't swap the store later. Every new primitive here is
built the opposite way:

1. **Define the port (interface) first**, then ship a **default adapter** behind it.
2. The engine depends on the **interface**, never the impl.
3. **No impl assumptions leak into the port** — no SQL in `JobQueue`, no filesystem paths in
   `BlobStore`. That leak is precisely what makes a thing un-swappable.

In C a port is a **struct of function pointers + an opaque `ctx`** (a vtable):

```c
typedef struct {
  void *ctx;
  int   (*put)(void*, const char *key, const void*, size_t);
  int   (*get)(void*, const char *key, buf_t *out);
  char *(*url)(void*, const char *key);
  int   (*del)(void*, const char *key);
} blob_store_t;          // default: local disk; later: S3/R2 — engine never knows which
```

Existing pattern to copy: **`core/mailer.c`** is already a notification-channel-shaped adapter —
push/SMS should sit beside it behind one port. Cautionary tale to *not* repeat: the
`app_db`/`db_sqlite` data layer (the coupling we're avoiding here).

## Independent modules — build + unit-test standalone, behind a port

No cross-dependencies → each can be built and tested **in isolation, before the app exists**.

| Module | Port | Default adapter | First needed |
|---|---|---|---|
| Blob storage | `BlobStore` (put/get/url/del) | local disk → S3/R2 + CDN | P1 (media) |
| Image processing | pure lib (decode/resize/thumbnail/encode) | — (no swap) | P1 |
| Image pHash | pure lib (hash/compare) | — (no swap) | P3 (dup/stolen-photo) |
| Jobs / scheduler | `JobQueue` (enqueue/process/cron/retry) | SQLite table + worker → Redis/NATS | P2 |
| Event ingest | `EventSink` (emit) | SQLite / append-log → stream/warehouse | P2 |
| Captcha | `CaptchaVerifier` (verify token) | Turnstile / hCaptcha | P2 |
| Notifications | `NotifChannel` (send) | email = `mailer.c` (have); **push** = Web-Push/FCM (new); SMS = optional | P2 (push) |
| Passkeys / WebAuthn | self-contained ceremony + `CredentialStore` port | credential storage behind port | P2 (optional) |
| Webhook intake | `verify()` fns (per provider) + `IdempotencyStore` port | SQLite | P4 (payments) |

## Coupled to the engine core — NOT standalone modules

These extend existing subsystems; they can't be side modules.

- **Faceted-query support** ⚠️ — *is* the data/query layer (SQL over the facet-index + FTS5). Lives
  **behind the data/search port**, not beside it. This is the **SQLite-adjacent coupling to design
  deliberately**: build it as part of that port from day one so a future SQLite→Postgres / search-engine
  swap takes it along. (Hand-write the SQL in a hook first if needed; promote to engine if it gets hot.)
- **Untrusted-code sandbox** ⚠️ — hardens the *existing* LuaJIT hook engine (`cel_hooks`/`cel_lua`);
  a modification of core hook execution, not a bolt-on. (Phase 5, far off.)

## Media is *mostly* modular (not a monolithic exception)

Split it — only the last part is coupled:
- **Blob storage** → the cleanest port of all (table above). ✅
- **Image processing** (resize/thumbnail) → a pure standalone lib. ✅
- **Upload + serve HTTP handlers** → the *only* coupled part (multipart, range requests, cache
  headers tie to the request loop) — thin glue over the two modular pieces.

> **Client-side pre-resize is a complementary *front-end* optimization, not a replacement.** The
> app (web/mobile) should downscale / compress / HEIC→JPEG / strip-EXIF *before upload* (saves
> bandwidth + UX, esp. mobile). But the client is untrusted, so the server-side image module stays
> **mandatory**: validate (size/dimension caps *before decode* → decompression-bomb guard), **re-encode**
> (neutralizes embedded exploits/polyglots; never serve user SVG as-is), generate the canonical
> variant set, and moderate (NSFW + pHash). Client optimizes; server validates and re-derives the
> source of truth. *(On-the-fly edge resize — Cloudflare Images / imgproxy — is a later serve-side
> swap; upload-time validate + re-encode is non-negotiable either way.)*

## Build order

`BlobStore` + image lib (P1) → `JobQueue` (P2) → `EventSink` (P2) → `CaptchaVerifier` (+ passkeys) (P2)
→ `NotifChannel`/push (P2) → image pHash (P3) → faceted-query *(in the data port)* (as hot) →
webhook verify + idempotency (P4) → untrusted-code sandbox (P5) → scale adapters (P6, measured need).

The independent modules (P1–P4 above, minus faceted-query) can be built **in parallel, each with its
own tests**, then wired in — the "engine-only first, tested in isolation" track.

## Notes

- The **data/search port** is the one big sticky seam (faceted-query, FTS, the catalog store all ride
  it). It's the hardest to retrofit and the most important to get right — see the swap-path table and
  the sharding north star in `classifieds-design.md` (Scaling section).
- Default adapters stay **embedded/simple** (SQLite, local disk, in-process); scale-out adapters
  (S3, Redis, external search) are built only on a *measured* need — same discipline as the
  portico-tunnel parked multi-core work.
