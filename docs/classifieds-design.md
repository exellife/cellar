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

## Storefront model & multi-tenancy — **DECIDED: hybrid / tiered**

The catalog is **one app**; storefronts are tiered presentation on top of it.

- **Global catalog app (single source of truth)** — ALL listings, sellers, buyers, events, search
  index, recs, messaging, trust/moderation in one DB. *Every* seller's listings live here,
  **including pro sellers'**, so cross-seller discovery is always trivial.
- **Tier 1 — branded storefront (default, free)** — `a.main.com` → the global app, filtered by
  `seller_id`, rendered with a per-seller **theme config** (logo / colors / banner / layout preset +
  featured listings). Pure presentation: no separate DB, no custom code.
- **Tier 2 — pro storefront (paid)** — a per-seller cellar **bundle** giving **custom layout +
  styling + custom logic (hooks)** — but it is a custom *face over the shared catalog*, **not a data
  silo**. It reads/writes listings through the catalog API, scoped to its `seller_id`.

> **Load-bearing rule:** *listing data always lives in the global catalog; the pro tier adds a
> custom presentation + logic layer, never a separate listings DB.* This is what buys Shopify-like
> custom shops AND a Lalafo-like cross-seller feed without the aggregation tax.

**What the pro tier introduces (design points):**
- **Untrusted-code sandboxing** *(top concern)* — pro sellers run custom LuaJIT hooks; must be
  sandboxed (no cross-seller data, no escape). **(engine)**
- **Scoped catalog API** — the pro bundle calls the global catalog in-process (one cellar process),
  scoped to its `seller_id` (`engine/api.c`). **(engine)**
- **Custom domains + TLS** — Tier 1 subdomains ride a wildcard `*.main.com` cert; a pro seller's own
  domain (`myshop.kg`) needs per-domain ACME provisioning. **(engine/ops)**
- **Theme system** for Tier 1 — config-driven branding without a bundle. **(app + engine)**
- **Provisioning** a pro bundle is per-paying-seller (low volume) → per-app cost is fine.

**Monetization tie-in:** the pro storefront *is* a paid tier (see §5.7 / §7) — custom shop = revenue.

## 5. Component gaps — what we need to build (ranked)

> Ranked by how central each is to *this* product (discovery + connection), not eBay's.

1. **Media / image pipeline** — *foundational; every listing is photos.* Upload, validate, size
   limits, resize + thumbnails, storage (local now, S3-like later), cache-friendly serving.
   cellar has only static *bundle* serving, no user uploads. **(engine)**
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
5. **Notifications: SMS + push** — *KG is phone-centric.* Phone-OTP signup, SMS for "new
   message" / alerts; web/mobile push. Email exists; SMS + push don't. **(engine)**
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
     - *Identity (most leverage):* **phone-verified posting** (SMS OTP, block VOIP/disposable);
       new-account limits (fewer/no high-value listings); velocity caps per phone/device/IP;
       device fingerprinting (sybil); ATO protection (MFA, breached-password).
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
   - **Build-first order:** (1) phone-verified posting + new-account/velocity limits → (2) report +
     auto-throttle + moderation queue + shadowban → (3) safety nudges + trust badges (~free) →
     (4) blocklists + price-anomaly + duplicate/image-hash → (5) risk scoring → (6) graph/ML later.
   - **Note:** this is the *consumer* that ties together four other gaps — SMS (§5.5), event
     tracking (§5.3), jobs (§5.4), image pHash (§5.1) — an argument for building those primitives well.
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
    starting a chat needs a logged-in, phone-verified account → kills bulk number-scraping and
    yields a real contact signal. WhatsApp as a `wa.me` deep-link convenience.
  - **Every contact logged as an event** (chat-started / number-revealed / whatsapp-clicked) →
    feeds recs (§5.3), trust/"responsive seller" badges, and demand measurement.
  - **Proxy / masked numbers** (Uber-style forwarding) = privacy gold standard but needs telephony
    → **future**, not launch.
  - *Why chat-primary:* it's also the moderation surface (§5.6) — the dominant off-platform scam is
    only detectable while the conversation is on-platform; an immediate "→ WhatsApp" push is itself a
    mild risk signal. Ties to WS realtime, SMS/push (§5.5), and jobs (§5.4) for notifications.
- **Monetization** — levers: featured/bump promotion (§5.7), pro-dealer subscriptions, and the
  **pro storefront tier** (custom shop, see Storefront model). *Which ship at launch* is still
  **OPEN**, but the pro storefront is now a committed paid feature on the roadmap.
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

## 9. Parking lot / open questions

- **Name** — TBD.
- **Localization** — Russian + Kyrgyz (both needed; Russian primary in cities).
- **Client** — web-first PWA vs native mobile apps (KG is mobile-heavy; most classifieds traffic
  is in-app). → OPEN
- **Launch categories** — autos, real estate, electronics, jobs, services, home goods…? → OPEN
- **Phone verification** — required to post? (anti-scam lever) → OPEN
