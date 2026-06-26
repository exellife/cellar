# cellar + classifieds — build tasks (ordered)

> **Status:** plan of record (2026-06-26). The durable, ordered task breakdown derived from
> [`classifieds-design.md`](classifieds-design.md) (roadmap) + [`engine-modules.md`](engine-modules.md)
> (ports + core-vs-addon line). When *executing* a phase, spin up live session tasks from it.

## How to read

- **[E]** = cellar engine module (generic, **port-first**: design port → default adapter → standalone tests).
- **[A]** = classifieds **bundle** (domain: schema / hooks / policies / API / UI).
- **Phase 0–2 are task-level** (actionable now); **Phase 3–6 are epic-level** (refine when reached).
- `◻` todo · `◑` in progress · `✓` done.
- **Engine-first within each phase**, and the **engine modules are standalone** → buildable in parallel.
- **UI tasks are parked** on the web-PWA-vs-native decision (§Open); everything else is client-agnostic.

---

## Phase 0 — Foundations *(MVP prerequisites; E + A in parallel)*

### Track E — engine modules (build standalone + tested, behind a port)
- ✓ **E0.1 `BlobStore` port + local-disk adapter + tests** — port (`put/get/exists/del/url/destroy`,
  content-type, opaque `ctx`); disk adapter (blobs/ + meta/ trees, atomic temp+fsync+rename, key
  traversal guard); 45-check test (roundtrip, missing, overwrite, large, concurrent), ASan-clean.
- ✓ **E0.2 Image-processing lib (pure) + tests** — *Decision: **stb_image** (vendored, zero deps;
  client handles HEIC→JPEG, so server rejects non-JPEG/PNG).* sniff by magic bytes; **dimension/area
  caps *before* full decode** (bomb guard, + stb's own backstop); decode→downscale-to-box→strip
  metadata→canonical **JPEG** re-encode; SVG/HEIC rejected. Tests: in-mem fixtures, bomb headers,
  malformed/oversized, thumbnail fit-box; ASan-clean. *Deferred: WebP/HEIC encode (libwebp/libheif
  behind same iface, measured need); EXIF-orientation re-apply (clients bake it in).*

### Track A — data model (client-agnostic backend)
- ◻ **A0.1** Schema: `category` + `category_attribute` (metadata layer, §6).
- ◻ **A0.2** Schema: `listings` (common typed cols + `attributes JSON`), **global UUID ids**, **region/country
  tag**, **currency + locale** fields (scaling insurance).
- ◻ **A0.3** Schema: `listing_facet` (derived) + indexes `(key,num)`/`(key,text)`; reference tables
  (geo oblast→city→district; later make/model).
- ◻ **A0.4** Hook: write-time **validation** (read `category_attribute` → validate submitted attrs).
- ◻ **A0.5** Hook: **facet-sync** (populate `listing_facet` from JSON for filterable attrs, in-txn).
- ◻ **A0.6** Seed: initial categories + attributes + **KG geo tree** (data).

---

## Phase 1 — MVP loop *(post w/ photos → browse/search → contact)*

### Track E
- ◻ **E1.1 Media upload handler** — multipart, **wire size cap**, → image lib (validate/process) → `BlobStore`.
- ◻ **E1.2 Media serve handler** — variant selection, cache headers (range if needed).

### Track A
- ◻ **A1.1** Listing CRUD (create/edit/delete/get) via cellar CRUD + hooks; attach photos.
- ◻ **A1.2** Metadata-driven **post-form contract** (API returns a category's attr schema for the form).
- ◻ **A1.3** **Search: FTS5** over listings — tokenizer **`unicode61` + `trigram`** (ru/ky); sync triggers.
- ◻ **A1.4** **Faceted filtering** — *design the **data/search port** here (the sticky seam)*; query =
  base filter ∩ `listing_facet` lookups ∩ FTS ids; facet counts (cacheable).
- ◻ **A1.5** Browse by category + geo filter; listing detail page (API).
- ◻ **A1.6** **Contact** — phone-reveal (login-gated) + reveal/contact **event**; basic WS chat (existing
  realtime) + inbox.
- ◻ **A1.7** Favorites.
- ◻ **A1.8** Auth wiring — OAuth + email magic-link (existing) + optional TOTP; login gate on contact/post.
- ◻ **A1.UI** *(PARKED on client decision)* — the web/mobile client consuming the above APIs.

> **Milestone:** a usable classifieds (backend + minimal client).

---

## Phase 2 — Lifecycle, identity hardening, events

### Track E *(JobQueue + EventSink are standalone — may be pulled into Phase 0 if running the engine track ahead)*
- ◻ **E2.1 `JobQueue` port + SQLite adapter + tests** — enqueue/claim/process, cron, retry/backoff,
  dead-letter, visibility timeout; worker loop; crash-safety tests.
- ◻ **E2.2 `EventSink` port + SQLite/append adapter + tests** — `emit` + query for consumers; batched writes.
- ◻ **E2.3 `CaptchaVerifier` port + adapter + tests** — Turnstile / hCaptcha.
- ◻ **E2.4 `NotifChannel` port + push adapter + tests** — Web-Push/VAPID (or FCM); fold existing
  `mailer.c` (email) + optional SMS behind the same port.
- ◻ **E2.5 Verify TOTP/MFA in a live app** — tests cover it; exercise enroll→challenge→recovery end-to-end.
- ◻ **E2.6 Passkeys/WebAuthn module + `CredentialStore` port + tests** *(optional this phase)*.

### Track A
- ◻ **A2.1** Listing lifecycle — auto-expiry / renew / sold via `JobQueue` (cron sweep).
- ◻ **A2.2** Notifications — "new message" / "expiring" / saved-search match → `NotifChannel` via jobs.
- ◻ **A2.3** Saved searches + alerts (jobs match new listings).
- ◻ **A2.4** **Event instrumentation** — emit view/click/favorite/contact/search via `EventSink`
  (*start collecting now* — recs need history).
- ◻ **A2.5** **Progressive-trust** posting gates + captcha on signup/post (compose `CaptchaVerifier` +
  rate-limit + generic trust/status fields).

---

## Phase 3 — Trust & safety + feed home *(epic-level)*
- **[E]** image **pHash** lib. **[A]** report → auto-throttle → moderation queue → shadowban; safety
  nudges + trust badges; blocklists + price-anomaly + duplicate/stolen-photo; then risk scoring.
- **[A]** **Feed home** — non-personalized ranking (fresh + near + popular).

## Phase 4 — Recommendations + monetization *(epic-level)*
- **[E/A]** recommendations — precompute similar/also-viewed/trending from events; personalize feed.
- **[E]** webhook verify + `IdempotencyStore`. **[A]** promotion payments (featured/bump/VIP via jobs).

## Phase 5 — Pro storefronts (Tier 2) *(epic-level)*
- **[E]** untrusted-code **sandbox** (LuaJIT hardening); scoped catalog API; custom-domain TLS (ACME);
  Tier-1 theme system. **[A]** pro storefront bundles.

## Phase 6 — Scale-out *(epic-level, measured need only)*
- swap adapters (S3 / external search / Redis); per-region deployment for KZ/UZ.

---

## Open decisions / parked
- **Client: web PWA vs native** — gates the UI tasks (A1.UI) only; everything else proceeds. (§9)
- **Image lib:** libvips vs stb (decided in E0.2).
- **Providers:** captcha (Turnstile/hCaptcha), push (Web-Push/FCM).
- **Data/search port shape** — designed in A1.4.
- **Name, launch categories, phone-verify-to-post** (§9).
