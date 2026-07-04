# [working name: TBD] — Product & Platform Design

> **Status:** Draft v0 (2026-06-26). Living document — we are actively polishing this.
> Captures the product concept and the cellar engine gaps it implies.

---

## 1. Concept

A **Kyrgyzstan-focused online classifieds platform**: Craigslist-style listings + modern
marketplace discovery + recommendations, built on the **cellar** engine. People post items and
services; others discover them (search / feed / recommendations) and **contact the seller
directly**. Deals happen **off-platform** — the product connects people, it does not transact.

**Reference points:** Lalafo and OLX (the incumbent KG / Central-Asia classifieds), Craigslist
(the model), Avito (Russia).

## 2. What it IS / what it is NOT

**IS** — a *discovery + connection* product. The loop is:
> **Post → Discover (search / feed / recs) → Contact (phone / chat) → transact off-platform.**

**Explicitly OUT of scope** (this is the deliberate simplification vs eBay/Amazon):
- ❌ No checkout / cart
- ❌ No in-platform payments **between users**, no escrow
- ❌ No order management / fulfillment / shipping / delivery
- ❌ No seller payouts, no money ledger between users
- ❌ No transaction disputes / refunds

The platform **connects**; it does not **transact or fulfill**. (The only money that may flow is
sellers paying *us* to promote a listing — see §5.7.)

## 3. Core user flows

- **Post a listing** — title, description, category, price (fixed / negotiable / free), photos,
  location, contact info, per-category attributes.
- **Discover** — browse categories, search + filter, personalized feed, recommendations
  ("similar", "also viewed", "trending near you"), saved searches + alerts.
- **Contact the seller** — reveal phone (call / SMS / WhatsApp) and/or in-app chat.
- **Manage** — my listings (edit / renew / mark sold / delete), favorites, profile, ratings.
- **Promote** *(optional)* — feature / bump / VIP a listing (paid).
- **Report / moderate** — flag scams, prohibited goods, spam.

## 4. Built on cellar — the baseline we already have

cellar covers the CRUD spine of this product today:
- **Listings & data** — generic CRUD, schema catalog, query builder, the policy/authz engine.
- **Accounts** — Argon2 auth, sessions, OAuth, MFA/TOTP, rate limiting, CORS.
- **Messaging / realtime** — WebSocket pub/sub (buyer↔seller chat, live updates).
- **Email** — transactional mailer.
- **Misc** — favorites & seller ratings (data model), reporting flags (data model), metrics,
  OpenAPI, multi-app, offline sync.

## Storefront model & multi-tenancy — **DECIDED: Tier-1 only (config-driven)**

> **Revised 2026-07-03.** The earlier plan was a two-tier model (Tier-1 config storefronts + a Tier-2
> "pro" tier of per-seller cellar bundles running custom seller-authored hooks). **Tier 2 is dropped**
> (deferred indefinitely). Rationale: it forced the two hardest, riskiest engine primitives —
> **untrusted-LuaJIT sandboxing** (a permanent security hazard: escape / resource exhaustion /
> cross-tenant leaks) and a **scoped cross-bundle catalog API** (which breaks cellar's clean
> one-DB-per-app model) — to serve a small advanced segment. Real marketplaces (Lalafo, Avito,
> Alibaba storefronts) give sellers **rich templates/config, not code**; a feature a seller needs is
> almost always one the *platform* should build for everyone. A config-driven Tier 1 delivers ~90% of
> the "custom shop" value at a fraction of the risk, keeps the app a **single bundle**, and never
> requires the sandbox / scoped-catalog work. Reversible: if a paying customer ever genuinely needs
> bespoke per-seller logic, Tier 2 can be revisited then — Tier 1 forecloses nothing.

The catalog is **one app** (one bundle, one DB); a storefront is a **filtered view + theme config**
on top of it — no separate DB, no custom code, ever.

- **Global catalog (single source of truth)** — ALL listings, sellers, buyers, events, search index,
  recs, messaging, trust/moderation in one DB. Every seller's listings live here, so cross-seller
  discovery is always trivial (no aggregation tax, no silos to federate).
- **Seller storefront (Tier 1)** — a seller's public page = the global app **filtered by `seller_id`**,
  rendered with a per-seller **theme config** (logo / colors / banner / layout preset + featured
  listings + about / socials / hours). Pure presentation over the shared catalog.

> **Load-bearing rule:** *listing data always lives in the one global catalog; a storefront is only a
> filtered, themed presentation of it.* Cross-seller feed and per-seller shop from the same rows.

**What Tier 1 needs (all within the single app — no sandbox, no cross-bundle access):**
- **Public seller-page rpc** *(the concrete gap)* — `seller {id}` → public profile block + that
  seller's active listings (paginated); plus a `seller` filter on `search`. **(app)**
- **Theme system** — a per-seller `seller_theme` record + set/read rpcs (config-driven branding).
  **(app; optional tiny engine help for asset serving)**
- **Custom domains** — a seller's own domain (`myshop.kg`) maps **Host → seller_id → their storefront
  view** (no per-seller bundle). Rides a wildcard `*.main.com` cert for subdomains; a custom apex
  needs per-domain **ACME**. **(engine/ops — but *only* the ACME piece, not the sandbox/scoped parts)**

**Monetization tie-in:** the paid "pro" lever is now **config-based** (custom domain + expanded theme
options + featured/boosted placement + more photos/listings + analytics), not custom code — same
revenue, far less engine risk. See §5.7 / §7.

**Cross-app note:** dropping Tier 2 also simplifies the planned B2B manufacturer marketplace — its
company pages become the same config-driven Tier-1 storefronts over a global catalog, so the
sandbox/scoped-catalog primitive is never needed there either.

## Admin panel / management surface — *(planned; thinking-through TBD)*

> Noted 2026-07-03. Management console for jarchy **and** the B2B app (they share the catalog, so
> **one admin panel serves both**). Key finding: **most of the backend already exists** — the panel is
> largely a *frontend*, not engine work.

**Already admin-manageable today (generic `/api` CRUD, `create/update/delete` = admin-only):**
`category`, `category_attribute` (the facet schema), `geo_oblast/geo_city/geo_district`. So adding a
category/attribute/geo is `POST /api/<table>` as admin **right now** — the data surface is live.
(tandem also already has its admin surface: `inquiries` / `set_inquiry_status` / `blocklist_add`.)

**Where the panel UI can live (all supported by cellar):**
1. **cellar's built-in admin UI** — the generic table console cellar already ships (fallback when a
   bundle has no `public/`). Zero build; the **interim** for raw category adds today. Not workflow-aware.
2. **Role-gated `/admin`** section of the main app, **or**
3. **A separate admin bundle** (`admin.svngn.com`) — cleanest; **one panel for jarchy + b2b**. ← recommended.

**Backend to *add* for a real panel (incremental admin rpcs — engine/app side, when we build it):**
- **Moderation** (cross-owner → dedicated rpcs, not CRUD): `takedown_listing`, `ban_user`, flagged-review queue.
- **Seller/company**: `verify_seller`; for b2b, company verification + membership management.
- **Taxonomy convenience**: category reorder + bulk import/seed (raw CRUD works; bulk is nicer as an rpc).
- **Dashboard**: stats rpcs off EventSink (`listing_viewed`, inquiries, …).
- Plus the already-scoped **`site_setting`** (editable contact/site config) and **public seller-page** rpcs.

**Recommendation:** a single **admin SPA** (jarchy + b2b) served as its own **role-gated bundle**,
driving the existing admin CRUD + the workflow rpcs above; use the **built-in admin UI as the interim**.
Next step when resumed: inventory exactly which admin rpcs exist vs. need building + spec the panel's
data contract (once, for both apps).

## Monetization & account tiers — *(design of record; activate with billing)*

> Noted 2026-07-04. NOT the storefront "Tier-1/Tier-2" decision (that's the *technical* page model,
> §Storefront). These are **subscription/account tiers** + à-la-carte promotion — the revenue model.

**Guiding principle: people pay to sell *faster*, not to list *more*.** An individual selling one phone
never hits a listing cap, so a quota is a weak upgrade lever for casual users — it only bites
professional/high-volume sellers. So the model has **two revenue surfaces**:

1. **À-la-carte promotion (the primary money-maker)** — per-listing paid visibility, sold to *everyone*
   (free users included), orthogonal to tiers: **bump** (re-sort to top), **featured** (badge +
   placement), **urgent**, **highlight**. This is where classifieds (OLX/Avito/Lalafo) actually earn.
2. **Subscription tiers (recurring)** — a *bundle* for professional sellers.

**Tiers: launch with 2, architect for 3.** (Don't ship 3 speculatively — no billing yet, no signal.)
| Tier (RU) | Audience | Bundle |
|---|---|---|
| **Free / Бесплатный** | individuals | base listing quota (the velocity limit today), standard expiry, in-app contact |
| **Pro / Про** | power sellers | bigger/longer listing quota + auto-renew, **storefront** (Tier-1), **analytics**, **promotion credits**, verified badge |
| **Business / Бизнес (or Магазин)** *(later)* | dealers, agencies, shops | company/team account + membership, bulk tools, custom storefront/domain, highest quota, priority |

> Naming: **Free / Pro / Business** (audience-named), **not** "Pro Max" (Apple-phone branding that says
> nothing about *who* it's for). The 3rd tier is the *business/shop* tier — name it so.

**Levers → engine bits that already exist (this is an extension, not a new subsystem):**
- **Listing quota / velocity** → the progressive-trust `POST_LIMIT_24H` limit (already scaled per user).
  Extend: `limit = base × plan_multiplier` — `plan` replaces/augments `trust` in the same code path.
- **Listing duration / auto-renew** → `EXPIRY_DAYS` per plan + the existing renew job.
- **Storefront** → the Tier-1 config-driven seller page (Phase 5).
- **Analytics** → `EventSink` (`listing_viewed` / contact events) → a per-seller dashboard rpc.
- **Verified-business badge** → `email_verified` + a business-verification step.
- **Promotion** → a `listing_promotion` table `(listing_id, type, expires_at)` + a search-ranking factor +
  a job to expire it; `EventSink` measures the lift. Sold as one-offs or Pro's monthly credits.

**Dependencies / sequencing:** payments are **not wired** (deferred, roadmap Phase 4). Until then this is a
*model*, not live: at launch everyone is effectively Free (the velocity limits already give the "basic
quota"). Activation needs a `plan` field (user/profile column or a `subscription` table, set by billing) +
the promotion table + ranking hook. **Decision: Free + Pro at launch; promotion à-la-carte is the money-
maker (not listing count); add Business when demand shows.**

## 5. Component gaps — what we need to build (ranked)

> Ranked by how central each is to *this* product (discovery + connection), not eBay's.

1. **Media / image pipeline** — *foundational; every listing is photos.* Upload, validate, size
   limits, resize + thumbnails, storage (local now, S3-like later), cache-friendly serving.
   cellar has only static *bundle* serving, no user uploads. **(engine)**
   - **Client-side pre-resize is a complementary front-end optimization, not a replacement:** the
     app should downscale/compress/HEIC→JPEG/strip-EXIF before upload (bandwidth + UX), but the
     server-side module stays mandatory — validate (caps before decode = bomb guard), **re-encode**
     (kills embedded exploits; never serve user SVG raw), canonical variants, moderate (NSFW + pHash).
2. **Search + faceted filtering + geo** — *the primary discovery surface.* FTS5 over listings +
   facets (category / price / location / attributes) + ranking. Geo: oblast → city → district.
   **(engine + app)**
   - **Tech: SQLite FTS5** (built-in, no separate search system). It builds an *inverted index*
     over listing text → fast, ranked search via `MATCH` with **BM25** relevance; supports
     phrase / prefix / boolean queries and `snippet()` highlighting. Kept in sync with the
     `listings` table via triggers / external-content.
   - **Facets are plain SQL**, not an FTS feature — count/filter per category, price bucket, city,
     and attributes using ordinary indexed columns *alongside* the FTS `MATCH`. Geo filtering
     (oblast → city → district) is also indexed columns.
   - **⚠️ ru/ky tokenizer:** FTS5 stemming is English-only. For Russian + Kyrgyz use `unicode61`
     (handles Cyrillic) **+ the `trigram` tokenizer** (any-3-char-substring → substring and
     typo-tolerant matching across both languages, no language-specific stemmer needed). This is
     the one real search decision to lock.
   - **Not** a distributed search platform (no clustering); for KG scale FTS5 + SQL facets is
     plenty. Revisit only if listing volume or query complexity outgrows it.
3. **Recommendations / personalized feed** — *the differentiator from plain Craigslist.* Needs (a)
   an **event-tracking pipeline** (views, clicks, favorites, contacts, searches) and (b) a
   **precompute layer** (similar-items, also-viewed, trending-in-city, affinity feed). KG scale =
   SQL co-occurrence + content similarity; no ML platform needed yet. **(engine + app)**
4. **Background jobs / scheduler** — *foundational for the lifecycle.* Listing auto-expiry
   (Craigslist's ~30-day rule), saved-search alerts, recommendation precompute, notification
   fan-out, cleanup. cellar has no job system. **(engine)**
5. **Identity & auth + notifications** — three layers, mostly already in cellar.
   **(engine — passkeys / captcha / push are new; OAuth / email / TOTP exist)**
   - **Auth = passwordless:** **passkeys/WebAuthn** (new — phishing-proof, no per-auth cost),
     **OAuth** Google/Apple (cellar has), **email magic-link** (cellar mailer). *No SMS front door,
     no Telegram.*
   - **Account security:** **TOTP/MFA** — *already in cellar* (covered by tests, **not yet exercised
     in a live app → verify before relying on it**); optional for users, encouraged/required for
     sellers & pro.
   - **Sybil-resistance:** **progressive trust + risk-routing + bot captcha** (e.g. Cloudflare
     Turnstile) — gate *posting* (esp. high-value) on earned trust, **not** a universal phone gate.
     Heavyweight verification (**phone / ID-KYC / card-on-file**) reserved for high-risk / high-value /
     verified-seller escalation only — phone is *one optional lever*, never required.
   - **Notifications (separate concern):** email (have) + **push (new)** + **SMS (optional**, for
     escalation/alerts only**)** — fanned out via jobs (§5.4). *SMS is demoted: not auth, not critical path.*
6. **Trust & safety / moderation** — *classifieds are scam magnets.* **(app + engine hooks)**
   - **Threat model (ours):** since money never flows through us, the dominant scam (advance-fee /
     prepayment) happens **off-platform** — we detect the setup, not the transaction. Real vectors:
     advance-fee, phishing links in chat, off-platform luring ("→ WhatsApp"), fake/stolen-photo
     listings, account takeover, spam/duplicates, prohibited goods, impersonation, review fraud.
     *KG advantage:* cash-on-pickup in public is the norm and is inherently scam-resistant.
   - **Two principles:** (1) raise the cost of *scam accounts* asymmetrically while keeping legit
     posting frictionless; (2) friction **proportional to risk** (a risk score gates challenges) —
     never one global gate. Defense in depth: heuristics + community + scoring + human review.
   - **Layers:**
     - *Identity (most leverage):* **progressive trust + bot captcha** (gate posting on earned trust,
       not a universal phone gate — see §5.5); new-account limits (fewer/no high-value listings);
       velocity caps per device/IP/account; device fingerprinting (sybil); ATO protection (TOTP/MFA,
       breached-password). Heavyweight verification (phone/ID/card) only as high-risk escalation.
     - *Post-time screening:* blocklists (prohibited goods + scam phrases + **phone/links in
       title/desc** → blocks off-platform luring); **price-anomaly** vs category median;
       duplicate/stolen-photo detection (text shingling + **image pHash**); image NSFW/illegal.
     - *Behavioral/graph:* **risk score** fusing all signals → allow/limit/review/block;
       cluster detection (shared device/IP/phone/image = scam ring); chat scam-pattern detection.
     - *Community:* easy **report** → N reports auto-throttle; **trust badges** (verified phone,
       age, ratings); in-context **safety nudges** ("never pay in advance / meet in public") —
       cheapest counter to the off-platform scam.
     - *Enforcement:* **risk-routed** moderation (low risk publishes, high risk → review queue);
       **shadowban**; graduated ladder warn→limit→suspend→ban (account **+ phone + device**);
       confirmed scams feed back into blocklists/hashes/model.
   - **Build-first order:** (1) progressive trust + captcha + new-account/velocity limits → (2) report +
     auto-throttle + moderation queue + shadowban → (3) safety nudges + trust badges (~free) →
     (4) blocklists + price-anomaly + duplicate/image-hash → (5) risk scoring → (6) graph/ML later.
   - **Note:** this is the *consumer* that ties together four other gaps — identity/captcha (§5.5),
     event tracking (§5.3), jobs (§5.4), image pHash (§5.1) — an argument for building those primitives well.
7. **Promotion payments** *(optional, monetization)* — sellers pay to feature / bump / VIP a
   listing, plus pro-dealer subscriptions. One-directional: *charge → set a flag → schedule
   expiry*. No ledger, no escrow, no payouts — a minor component. **(app + engine webhook intake)**

**Supporting:** an event/analytics ingestion path (feeds recs, moderation, and business metrics)
— overlaps with #3 and #4.

## 6. Category & attribute data model — **DECIDED**

**Category taxonomy + per-category structured attributes + faceted filtering** (a car has
year/mileage/engine; an apartment has rooms/area/floor). This drives the post form *and* the filters
*and* search *and* recs. **The unlock is two separate layers** (people conflate them):

**Layer 1 — categories & attributes as *data*, not code.** The tree and each category's attribute
schema are rows, so admins add a category + its fields with **no migration / no deploy**:

```
category(id, parent_id, name, slug, …)                     -- the tree
category_attribute(
  category_id, key, label, type,        -- type: enum|int|number|bool|text
  required, filterable, unit,           -- filterable = appears as a facet
  options[], sort_order, depends_on)    -- options = small enums; depends_on = make→model
```

This one table **generates** the dynamic post form, the filter/facet UI, and write-time validation.
No per-category code. (Same regardless of storage choice below.)

**Layer 2 — storage: JSON values + a derived typed facet-index.** Surveyed: pure EAV (filters =
N self-joins, slow ❌), per-category tables (DDL migration per category, fights admin-defined
categories ❌), JSON column (flexible ✅ but only fast if paths are indexed). Chosen:

```
listings(id, seller_id, category_id, title, description,
         price, currency, city_id, district_id,           -- COMMON fields = first-class indexed cols
         status, condition, created_at, expires_at,
         attributes JSON)                                  -- category-specific values

listing_facet(listing_id, key, num_value, text_value)      -- ONE row per *filterable* attr, derived
  -- indexes: (key, num_value), (key, text_value)
```

The facet table is *typed EAV but only for filterable attrs*, derived from the JSON → JSON's
flexibility for storage/display **and** fast faceted filtering, and it generalizes across
heterogeneous categories (unlike fixed generated columns).

- **Common fields first-class** (price, geo, condition, category, status, dates) — filtered/sorted
  on every query, so real indexed columns, not JSON.
- **Big taxonomies = reference tables**, not enum lists: car **make/model** (`model(make_id)` handles
  the dependency), and **geo** (`oblast→city→district`, `city_id`/`district_id` first-class FKs).

**Query pattern** ("cars, Toyota, 2010–2018, <150k km, Bishkek, newest"):
> base `listings` filter (category/city/status) **∩** indexed `listing_facet` lookups
> (`make='Toyota'`, `year BETWEEN…`, `mileage<…`) → intersect ids → order. Facet **counts**
> ("Toyota (1,234)") = `GROUP BY key` over the set (cacheable). **FTS5** produces candidate ids →
> intersect with the same facets.

**Write path:** a cellar **hook** reads `category_attribute` to validate the submitted attributes
(required / type / enum / range), then writes the JSON **and** upserts `listing_facet` rows for
filterable attrs — one transaction, fully data-driven.

**cellar fit:** storage / JSON / triggers / reference tables / validation+sync hooks are buildable
today. The one spot that may want **engine support** is efficient *multi-facet filtering + facet
counts* blended with FTS — hand-write the SQL in a hook first; a generic faceted-query capability in
the query builder is a *later* engine investment if it proves hot. **Scale:** KG volumes
(tens of thousands active, low-millions lifetime) sit comfortably in SQLite + these indexes — no
separate search/OLAP system needed.

## 7. Open design decisions (forks)

- **Discovery model** — ~~search-first vs feed-first~~ → **DECIDED: search foundation + feed home,
  recs as fast-follow.** Search (FTS + facets + categories) is mandatory infrastructure either way
  and is the foundation. The **home is a feed**, but v1 ranks on **fresh + near you + popular-in-city**
  (non-personalized → works with thin inventory, no cold-start). Personalized recs (§5.3) layer on as
  event data accumulates — a fast-follow, not a launch blocker. (Rationale: search serves high-intent
  users; the feed drives the engagement/habit Lalafo won on; starting the feed dumb sidesteps the
  empty-marketplace chicken-and-egg.)
- **Contact model** — ~~phone vs chat vs both~~ → **DECIDED: both, in-app chat primary, phone
  privacy-protected.**
  - **Both channels**, with **in-app chat as the default/prominent** action (uses the existing WS
    realtime); phone secondary.
  - **Seller controls** which channels a listing exposes (chat / call / WhatsApp).
  - **Phone reveal gated behind login** — browsing is free (SEO), but seeing the number *or*
    starting a chat needs a logged-in, trusted account → kills bulk number-scraping and
    yields a real contact signal. WhatsApp as a `wa.me` deep-link convenience.
  - **Every contact logged as an event** (chat-started / number-revealed / whatsapp-clicked) →
    feeds recs (§5.3), trust/"responsive seller" badges, and demand measurement.
  - **Proxy / masked numbers** (Uber-style forwarding) = privacy gold standard but needs telephony
    → **future**, not launch.
  - *Why chat-primary:* it's also the moderation surface (§5.6) — the dominant off-platform scam is
    only detectable while the conversation is on-platform; an immediate "→ WhatsApp" push is itself a
    mild risk signal. Ties to WS realtime, SMS/push (§5.5), and jobs (§5.4) for notifications.
- **Monetization** — levers: featured/bump promotion (§5.7), pro-dealer subscriptions, and a
  **config-based "pro" storefront tier** (custom domain + expanded theme + featured placement +
  analytics — Tier-1 only; see Storefront model). *Which ship at launch* is still **OPEN**. (The
  earlier *custom-code* pro tier / Tier 2 is dropped — see the Storefront model revision.)
- **Moderation** — ~~pre vs post-publish~~ → **LEANING: risk-routed** (low risk publishes
  immediately, high risk → review queue), per §5.6. Confirm the risk thresholds later.

## 8. Architecture & scale

A classifieds platform is **one app, one DB** (all users/listings/events in one place). cellar's
multi-app-per-SQLite model → a single-app deployment: ~5k writes/s single-writer, ~57k reads/s.
For Kyrgyzstan scale (≈6.5M people; realistically thousands concurrent early) this is **comfortably
enough** — the constraint is the missing primitives above, not throughput. Scale-out is a later
problem (see next section for the *international* path).

## Scaling & internationalization strategy

> Principle: **don't build the large-scale version — build the *seams* so we can swap into it.**
> Cheap insurance (discipline), not speculative infrastructure.

**North star — shard by country, not "make one system infinite."** International expansion
(KZ, UZ, …) and scale point to the *same* answer: each country = its own catalog/deployment.

- Turns "infinite scale" into **"more SQLite-sized regions"** — each country stays in the
  comfortable single-box envelope; scale by *adding regions*, not by scaling one DB forever.
- It's **cellar's natural shape** (multi-app, multi-DB, per-tenant).
- **Cross-region queries are rare** (a Bishkek buyer won't browse Almaty's couches) → sharding by
  region is cheap precisely because we almost never need cross-shard joins.
- Matches reality anyway: per-country **language** (ky/kk/uz/ru), **currency** (KGS/KZT/UZS), geo
  tree, and **data-residency law** (KZ/UZ may require local data).
- → "Large scale" becomes an *operational* story (more regions), not a re-architecture. A thin (or
  no) global layer handles the rare cross-border need; user identity is per-region (global SSO later
  if needed).

**Bake in NOW (painful to retrofit, cheap today):**
1. **Region/country tag on every entity** — split by country later with no migration.
2. **Global IDs (UUID / region-prefixed), not per-DB autoincrement** — autoincrement collides the
   moment you shard or merge. The subtle one people regret.
3. **Statelessness** — app nodes stateless; session/cache/realtime state must be *externalizable*
   (even if v1 keeps it in-process). The gate to horizontal scale.
4. **Multi-currency + locale in the model from the start** (needed for international regardless).
5. **Go through cellar's data/API seams, not raw SQL in hooks** — so the storage engine *can* change.

**Component swap-paths — the §5 gaps ARE the seams.** Build each primitive as an interface with a
simple embedded default; the scale-out adapter is then a config + adapter, not a rewrite:

| Component | Embedded default (now) | Scale-out adapter (later) |
|---|---|---|
| Object storage | local disk | S3-compatible (R2/MinIO) + CDN |
| Search | SQLite FTS5 | Meilisearch / Typesense / OpenSearch |
| Jobs / queue | SQLite-backed table | Redis / NATS |
| Cache + session | in-process / SQLite | Redis (shared across nodes) |
| Realtime pub/sub | in-process | Redis pub/sub / message bus (cross-node WS) |
| Events / recs | SQL co-occurrence | event stream → warehouse → model |
| Email / SMS / push | provider SDK | (already external — config only) |
| **Catalog DB** | **SQLite per region** | **Postgres** *(only for a single hot region)* |

**The one sticky seam:** primary store **SQLite → Postgres** (SQL dialect / txns / query builder
couple tightly; cellar is SQLite-shaped). Region-sharding is exactly what lets us *avoid* it — only
a *single country* outgrowing a box would force it, far away and isolated to that region.

**Discipline:** build the *interfaces* + the cheap structural insurance now; do **not** build the
scale-out adapters until a *measured* need (same rule as the tunnel's parked multi-core work).
Over-abstraction is its own failure mode.

### Write scaling — the real SQLite constraint (not RAM/file size)

SQLite reads scale fine well past RAM (B-tree working-set, not whole-file, in cache; graceful on NVMe —
~57k reads/s measured). The actual ceiling is the **single writer** per DB file (~5k write-txns/s with
FTS + hooks; WAL + `synchronous=NORMAL` so commits don't each fsync). Everything writing to one
`data.db` shares that one lock. Write volume, highest first:

1. **Events** (`listing_viewed` on *every* view, `search`, …) — the firehose, and **append-only /
   offloadable**.
2. **Chat** — #2: each message is ~3 write-txns (message + `conversation.last_message_at` bump +
   `notification`). Lower volume than views (people view 100s of listings per message), but amplified.
3. Posts / favorites / jobs — trickle.

Fixes, cheapest first — **all are adapter/structural changes, no engine refactor** (the seams exist):

- **Events → batch, then split.** Buffer emits in memory + flush every N / ~200ms in one txn (1000
  events → 1 fsync; ~1000× fewer write-txns) — stays in SQLite, zero deps. If still hot, point the
  **`EventSink` port** at its own DB file, then an append-log / stream. *If you ever go embedded-KV for
  the firehose, use **RocksDB** (LSM, write-optimized) — **not LMDB**, which is single-writer +
  read-optimized and buys nothing for writes.* Events are a queue, not the source of truth: a rollup
  job aggregates them back into SQLite (the queryable counts recs/dashboards read).
- **Chat → separate, don't KV it.** Messages need relational queries + realtime `owner_via` scoping +
  policy, so they can't move to a KV store. The scale move is **separation**: chat's tables in their own
  SQLite *file* (own write lock), then chat as its own **cellar app/bundle** — the multi-app/multi-DB
  architecture makes this natural; region-sharding already bounds it. Within SQLite, trim the
  `after(message)` amplification (batch the `last_message_at` bump / coalesce rapid notifications).

Order of operations when write pressure shows: **batch events → split events store → split chat DB.**
None needed at KG/MVP volume (both fit comfortably under the single-writer budget); measured-need only.

## Build phases / roadmap

> Ordering: **MVP loop first → infra where it unblocks → defer optional/monetization/scale.**
> Tags: **[E]** cellar engine (reusable primitive), **[A]** classifieds app on cellar.

**Phase 0 — Foundations.** §6 data model (category/attribute metadata + listings + JSON attrs +
facet-index + geo reference tables; validation + facet-sync hooks) **[A]**; structural insurance —
region tag, **global UUIDs**, multi-currency/locale **[A/E]**.

**Phase 1 — MVP loop** (a working classifieds: post w/ photos → browse/search → contact).
- **[E] Media pipeline** — upload/validate/store (local disk *behind a storage interface*)/resize/serve.
- **[A]** Listing CRUD (metadata-driven post form + photos); browse by category + **FTS5 search +
  facets + geo**; listing detail + contact (phone-reveal login-gated + basic WS chat) + favorites.
- **Auth:** existing **OAuth + email magic-link** (+ optional **TOTP**) — *no new identity work to launch.*

**Phase 2 — Lifecycle, identity hardening, events.**
- **[E] Jobs/scheduler** (job table + worker + cron + retries) — the infra primitive.
- **Identity (mostly existing):** verify **TOTP** in a live app; add **passkeys** **[E]** if desired;
  add **bot captcha** **[E]** + **progressive-trust** posting gates **[A]**. *SMS NOT built here —
  demoted to optional escalation/alerts; no Telegram.*
- **[E] Event tracking** ingest — start collecting views/clicks/favorites/contacts *now* (recs need history).
- **[A]** Listing lifecycle (auto-expiry/renew/sold via jobs); notifications (email[have] + **push[E]**)
  via jobs; **saved searches + alerts**.

**Phase 3 — Trust & safety + feed home.**
- **[A/E]** Trust per §5.6 order: report → auto-throttle → moderation queue → shadowban; nudges + trust
  badges; blocklists + price-anomaly + duplicate/stolen-photo (**image pHash [E]**); then risk scoring.
- **[A] Feed home** — non-personalized ranking (fresh + near + popular).

**Phase 4 — Recommendations + monetization.**
- **[E/A] Recommendations** — precompute similar/also-viewed/trending from accumulated events; personalize feed.
- **[E]** Webhook intake + idempotency; **[A]** promotion payments (featured/bump/VIP via jobs).

**Phase 5 — Seller storefronts (Tier 1, config-driven).** *(Tier 2 dropped — see Storefront model.)*
- **[A]** public **seller-page rpc** (profile + seller's active listings) + a `seller` filter on `search`;
  per-seller **theme** (`seller_theme` + set/read rpcs); featured/boosted placement.
- **[E]** custom-domain mapping (Host → seller_id) + per-domain **ACME** for custom apexes.
  *No untrusted-code sandbox, no scoped cross-bundle catalog API — those were Tier-2-only.*

**Phase 6 — Scale-out (measured need only).** Swap adapters (S3 / external search / Redis) +
per-region deployment for KZ/UZ (the swap-path table).

**cellar engine build order:** media pipeline → jobs → event ingest → captcha (+ passkeys) → push →
image pHash → faceted-query support (if hot) → webhook/idempotency → custom-domain ACME → scale
*(untrusted-code sandbox removed — it was Tier-2-only)*
adapters. *(SMS dropped from the critical path; TOTP already exists.)*

## 9. Parking lot / open questions

- **Name** — TBD.
- **Localization** — Russian + Kyrgyz (both needed; Russian primary in cities).
- **Client** — web-first PWA vs native mobile apps (KG is mobile-heavy; most classifieds traffic
  is in-app). → OPEN
- **Launch categories** — autos, real estate, electronics, jobs, services, home goods…? → OPEN
- **Phone verification** — required to post? (anti-scam lever) → OPEN
