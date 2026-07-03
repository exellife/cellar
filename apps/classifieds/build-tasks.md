# cellar + classifieds — build tasks (ordered)

> **Status:** plan of record (2026-06-26). The durable, ordered task breakdown derived from
> [`classifieds-design.md`](classifieds-design.md) (roadmap) + [`engine-modules.md`](../../docs/engine-modules.md)
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

### Track A — data model (client-agnostic backend) — bundle at `apps/classifieds/`
- ✓ **A0.1** Schema: `category` + `category_attribute` (metadata layer, §6).
- ✓ **A0.2** Schema: `listings` (common typed cols + `attributes JSON`), **uuid4 DEFAULT ids**, **region
  tag**, **currency + locale** fields (scaling insurance).
- ✓ **A0.3** Schema: `listing_facet` (derived) + indexes `(key,num)`/`(key,text)`; geo reference tables
  (oblast→city→district). *(make/model deferred — enum+depends_on for now.)*
- ✓ **A0.4** Hook: write-time **validation** in `before()` (read `category_attribute` → required/type/enum;
  rejects in-txn → 400, rolls back).
- ✓ **A0.5** Hook: **facet-sync** in `after()` (rebuild `listing_facet` from stored JSON via
  `json_extract`, idempotent; admin `rpc rebuild_facets` repair). *(after(), not before — id known
  post-commit; validation stays atomic with the write.)*
- ✓ **A0.6** Seed: KG geo tree + starter taxonomy (24 attrs, all types) + runnable `run.sh`/README.

> **Phase 0 done.** Engine: `BlobStore` + image lib. Bundle: full catalog data model + validation/
> facet-sync hooks + KG seed. e2e ctest `classifieds_phase0` (25 checks) + unit tests, all green.

---

## Phase 1 — MVP loop *(post w/ photos → browse/search → contact)*

### Track E
- ✓ **E1.1 Media upload handler** — `POST /media` raw image body (cap exempt from JSON cap, own
  `CEL_MEDIA_MAX`), → image lib validate/re-encode → JPEG variants (full/thumb) → per-app `BlobStore`.
- ✓ **E1.2 Media serve handler** — `GET /media/<id>/<variant>`, immutable cache, 302 for URL adapters.
  *(ctest `media`, 19 checks, ASan-clean. Generic engine endpoint — no domain leak.)*

### Track A
- ✓ **A1.1** Listing CRUD via cellar CRUD + hooks; **photos** JSON array (media ids, server-validated).
- ✓ **A1.2** Metadata-driven **post-form contract** — rpc `category_form` (category + breadcrumb + attrs).
- ✓ **A1.3** **Search: FTS5** over listings — `unicode61` (Cyrillic case-fold + prefix, bm25), sync
  triggers, rpc `search` (injection-safe). *(trigram substring = noted later enhancement.)*
- ✓ **A1.4** **Faceted filtering** — rpc `search` composes base ∩ `listing_facet` (text IN / num range)
  ∩ FTS ids, ranked + paged + sidebar facet counts. *Hook SQL (doc's "hook-first"); filters as an
  array (proxy has no enumerable keys). Promote to an engine data/search port if it gets hot.*
- ✓ **A1.5** Browse (= `search` with empty q + category/city) + listing detail rpc `listing`
  (anon-readable; engine /api reads require auth — see below).
- ✓ **A1.6** **Contact** — phone-reveal (login-gated; number in a separate gated table, no public leak)
  + channel flags + `contact_event` log + `set_listing_contact`/`reveal_contact` rpcs. **Chat**:
  conversation + `conversation_member` (VIA membership table) + message; `owner_via` scopes message
  reads AND realtime subscribe to participants (no `/api`/WS leak); `before()` gates message create
  on membership; rpcs `start_conversation`/`inbox`. WS e2e proves live delivery + non-member denied.
- ✓ **A1.7** Favorites — `favorite` table + toggle rpcs (`favorite`/`unfavorite`/`favorites`);
  `favorited` + `favorite_count` (social proof) on listing detail.
- ◑ **A1.8** Auth wiring — ✓ **gate audit** (caught + fixed: listings update/delete now `owner_column`
  seller_id → users edit/delete only their own; verified non-owner→404, forged seller_id ignored, anon
  write→401). ✓ **OAuth** is config-only (federated user → role `user`; `CEL_OAUTH_GOOGLE_*` documented
  in run.sh). ✓ password+session confirmed. ◻ **email magic-link** deferred to ops (needs domain +
  transactional provider + SPF/DKIM/DMARC — never direct-send).
- ◻ **A1.UI** *(PARKED on client decision)* — the web/mobile client consuming the above APIs.

> **Milestone:** a usable classifieds (backend + minimal client).

---

## Phase 2 — Lifecycle, identity hardening, events

### Track E *(JobQueue + EventSink built early — the async backbone, pulled ahead of the rest of Phase 2)*
- ✓ **E2.1 `JobQueue`** — module (atomic-claim/retry/visibility/dead-letter/recurring + `jobq_run_due`,
  27-check test) **+ WIRED + AUTONOMOUS**: per-app queue on a dedicated connection, `cellar.enqueue_job`
  Lua API, `cel_hooks_run_jobs` dispatch to the `job` hook, `POST /jobs/run` (admin) trigger, AND a
  background worker thread (`cel_worker`, `CEL_JOBS_INTERVAL`) driving it on a timer. Bundle:
  `expire_listings` sweep — e2e-proven to run autonomously (ctest `classifieds_worker`).
- ✓ **E2.2 `EventSink`** — module (`emit` + cursor `read`, 19-check test) **+ WIRED**: per-app sink on a
  dedicated connection, `cellar.emit` Lua API. Bundle emits `listing_viewed` (anon incl.) + `search`.
  e2e-proven (data collecting from day one). ◻ *later: cursor `read` consumed by a rollup job (recs).*
- ⊘ **E2.3 `CaptchaVerifier`** — **DEFERRED (measured need).** Not built: the high-value defenses
  (login-gated contact reveal kills number-scraping; rate-limit + lockout; OAuth signup; progressive
  trust A2.5; moderation) cover launch, and captcha is weak (AI/solver-farms) + adds signup/post
  friction. Keep the *seam*: when real bot abuse appears, a Turnstile adapter behind a small port is a
  ~½-day add gating signup/post. Don't pre-build.
- ◻ **E2.4 `NotifChannel` port + push adapter + tests** — Web-Push/VAPID (or FCM); fold existing
  `mailer.c` (email) + optional SMS behind the same port.
- ✓ **E2.5 TOTP/MFA verified live** — the engine `mfa` ctest is a full live e2e (enroll→confirm→two-step
  login challenge→verify→recovery codes→regenerate→disable); MFA is engine-level (`/auth/mfa/*`) so the
  classifieds bundle inherits it with zero bundle code. Confirmed.
- ⊘ **E2.6 Passkeys/WebAuthn** — **DEFERRED (optional).** OAuth + password + TOTP cover auth; revisit if
  passwordless-by-passkey becomes a priority.

### Track A
- ✓ **A2.1** Listing lifecycle — auto-expiry (recurring `expire_listings` sweep, seeded by run.sh, runs
  via the worker), `renew_listing` rpc (reset window + reactivate), sold/withdrawn via owner PATCH.
- ◑ **A2.2** Notifications — ✓ **in-app feed** (the bell): `notification` table (owner-scoped, realtime),
  `notify()` helper (persist + `cellar.rt_emit` live push), wired into new-message + listing-expired
  + saved-search match; rpcs `notifications`/`unread_count`/`mark_read`. ◻ off-site delivery via
  `NotifChannel` (Web Push / email) — see E2.4.
- ✓ **A2.3** Saved searches + alerts — `saved_search` table + rpcs; recurring `match_saved_searches`
  job (FTS+category+city since a last_run_at cursor) → in-app alert. *(facet-filter matching later.)*
- ✓ **A2.4** **Event instrumentation** — `EventSink` emits `listing_viewed`/`search`/`favorite`/`contact`
  (collecting from day one — recs need history). *(click events come with the web client.)*
- ✓ **A2.5** **Progressive trust + velocity limits** — trust from account age + email-verified (1×/3×/10×,
  no new table); per-24h post cap + per-1h contact cap scale with trust (admins exempt; env-tunable);
  `my_limits` rpc for the UI. Stops the spam funnel; captcha stays deferred (E2.3).

---

## Phase 3 — Trust & safety + feed home *(epic-level)*
- **[E]** image **pHash** lib. **[A]** report → auto-throttle → moderation queue → shadowban; safety
  nudges + trust badges; blocklists + price-anomaly + duplicate/stolen-photo; then risk scoring.
- **[A]** **Feed home** — non-personalized ranking (fresh + near + popular).

## Phase 4 — Recommendations + monetization *(epic-level)*
- **[E/A]** recommendations — precompute similar/also-viewed/trending from events; personalize feed.
- **[E]** webhook verify + `IdempotencyStore`. **[A]** promotion payments (featured/bump/VIP via jobs).

## Phase 5 — Seller storefronts (Tier 1, config-driven) *(epic-level)*
> **Tier 2 (per-seller custom-code bundles) dropped — see classifieds-design.md Storefront model.**
> No untrusted-code sandbox, no scoped cross-bundle catalog API.
- **[A]** public **seller-page rpc** (profile + seller's active listings) + `seller` filter on `search`;
  per-seller **theme** (`seller_theme` + set/read rpcs); featured/boosted placement.
- **[E]** custom-domain mapping (Host → seller_id) + per-domain **ACME** (the only engine piece).

## Phase 6 — Scale-out *(epic-level, measured need only)*
- swap adapters (S3 / external search / Redis); per-region deployment for KZ/UZ.

---

## Open decisions / parked
- **Client: web PWA vs native** — gates the UI tasks (A1.UI) only; everything else proceeds. (§9)
- **Image lib:** libvips vs stb (decided in E0.2).
- **Providers:** captcha (Turnstile/hCaptcha), push (Web-Push/FCM).
- **Data/search port shape** — designed in A1.4.
- **Name, launch categories, phone-verify-to-post** (§9).
