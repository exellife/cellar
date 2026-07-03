# classifieds — front-end integration guide

For whoever builds the client (web first, mobile later). The backend is the
cellar bundle in this directory; this doc is the contract + the non-obvious
rules. Read the **Gotchas** section before writing any fetch call.

## Architecture & stack

- **One origin.** cellar serves both the API and the app's static front-end
  (from `public/`). The web SPA is built and dropped into `public/`. No second
  host, no CORS in the simple setup.
- **API shapes:** plain **REST** (`/api/<table>`), **RPC** (`POST /rpc/<fn>`),
  and a binary **WebSocket** for realtime chat. All JSON.
- **Stack decision:** web = **React (Vite SPA)**, mobile (later) = **React
  Native (Expo)**. Write the cellar API client + TS types **once** as a shared
  module and reuse it across web and RN. SEO is deferred (plain SPA now); keep
  routes clean (`/`, `/c/:category`, `/l/:id`) so SSR/SSG can be retrofitted.
- **Auth transport:** a bearer token in `Authorization: Bearer <token>`. No
  cookies. Store the token (web: memory + refresh, or localStorage for the MVP;
  RN: SecureStore).
- **Charts/stats (when dashboards land):** a charting lib (Victory for web+RN
  parity, or Recharts/visx web-only) is **~100–130 KB gzipped** and MUST live
  behind a **lazily-loaded** stats/dashboard route (`React.lazy` / dynamic
  import) — never imported into the core browse/search/post bundle. Import
  modular (`victory-bar`, not the `victory` umbrella). A chart lib in the main
  entry is the one easy way to bloat first paint; behind a lazy route it costs
  ~0 KB for users who never open stats.

## Roles

- **anon** (no token) — may browse: categories, geo, listings (read), search,
  listing detail, media. Nothing that writes or reveals contact.
- **user** (logged in) — may post, edit/delete **their own** listings, reveal
  contact, chat, favorite.
- **admin** — superuser; curates the catalog (categories/attributes/geo). Not a
  normal app user.

## Auth endpoints

| Call | Body | Returns |
|---|---|---|
| `POST /auth/register` | `{email, password}` | the created user (role `user`) |
| `POST /auth/login` | `{email, password}` | `{token, ...}` |
| `POST /auth/oauth` | provider ID token (e.g. Google) | `{token, ...}` |

- **Google sign-in:** the client obtains a Google **ID token** (Google Identity
  Services) and POSTs it to `/auth/oauth`; cellar verifies it against Google's
  JWKS and creates/links the user as role `user`. No email round-trip. (Server
  must have `CEL_OAUTH_GOOGLE_*` configured — see `run.sh`.)
- **Email magic-link / verify-email:** NOT enabled yet (deferred — needs a
  domain + transactional email provider). Don't build flows that depend on
  receiving email until told it's live.
- **Password rules:** min 8 chars (server-enforced; mirror client-side).

## Gotchas — read this first

1. **JSON columns come back as STRINGS on `/api` reads.** `listings.attributes`
   and `listings.photos` are returned as JSON *text*, e.g.
   `"attributes": "{\"make\":\"Toyota\",\"year\":2015}"`. **`JSON.parse` them.**
   (The RPCs that hand-pick columns may already give arrays — see each.) When
   you POST/PATCH, send them as real nested JSON (objects/arrays); the engine
   serializes them.
2. **The phone number is NEVER in a listing read.** Listing rows carry only the
   channel *flags* (`allow_chat`/`allow_call`/`allow_whatsapp`). To get the
   actual number, call **`reveal_contact`** (login-gated, logged as an event).
   Show "Show number" / "Chat" / "WhatsApp" buttons from the flags; fetch the
   number on click.
3. **Login gates:** posting, editing/deleting, `reveal_contact`, chat
   (`start_conversation` + sending messages), and favorites all require a token.
   Gate the UI; the server returns 400/401/403 otherwise.
4. **Owner-only edit/delete.** A user editing/deleting a listing that isn't
   theirs gets **404** (not 403 — the row is invisible to the scope). Only show
   edit/delete on the owner's own listings.
5. **Search `filters` is an ARRAY, not an object.** See Search below. Each entry
   is `{key, values:[...]}` (enum/text, OR within) or `{key, min?, max?}`
   (numeric range). Multiple entries AND together.
6. **Photos are a two-step upload.** Upload each image to `POST /media` → get an
   `id` back → put the ids in `listings.photos` (array, **max 12**). Never send
   raw image bytes to `/api/listings`.
7. **Money:** `price` is an **integer in whole currency units** (e.g. soms — not
   cents). `currency` is a separate code (default `KGS`). `price_negotiable`
   is `0/1` ("договорная").
8. **Attributes are category-driven.** Don't hardcode car/apartment fields.
   Fetch the field schema from **`category_form`** and render the post form +
   filters from it. The server validates submitted attributes against it.
9. **Error shape.** Non-2xx REST → `{"status":"error","message":"..."}`. RPC
   success → `{"status":"ok","result":{...}}`; RPC failure → HTTP 400 with the
   error body. A denied read is **401** when logged-out, **403** when logged-in.
10. **Rate limits** → HTTP **429** on auth + data endpoints; back off + surface a
    "try again" message.

## Catalog (public)

- **Top categories:** `GET /api/category?where={"parent_id":{"is":null}}&order=sort_order`
- **Subcategories:** `GET /api/category?where={"parent_id":{"eq":"cat-cars"}}&order=sort_order`
- **Geo:** `GET /api/geo_oblast`, `GET /api/geo_city?where={"oblast_id":{"eq":"ob-chuy"}}`,
  `GET /api/geo_district?where={"city_id":{"eq":"ci-bishkek"}}`
- **Localized labels:** `name` is the default (ru); `labels` is JSON `{"ky":...}`
  (string — parse) when present.

**Post-form contract** — the one call that drives the dynamic post form AND the
filter sidebar:

```
POST /rpc/category_form { "category": "cat-cars" | "cars" }   // id or slug
→ result: {
    category:   { id, slug, name, labels },
    breadcrumb: [ {id, slug, name}, ... ],                    // root → leaf
    attributes: [ {                                           // ordered
      key, label, labels, type,            // type: enum|int|number|bool|text
      required, filterable, unit,          // booleans; unit e.g. "км"
      options,                             // array of strings (enum only)
      depends_on                           // key of a parent attr (model→make)
    }, ... ]
  }
```

## Listings (CRUD)

- **Public browse → use the `search` RPC** (below), never `GET /api/listings`.
  The generic `/api/listings` read surface is **owner-scoped**: a logged-in user
  sees only **their own** listings (any status — their drafts/sold), and anon is
  denied (401). That's the "My listings" endpoint, not the public catalog —
  `search` (status=active only) is the public browse/feed.
- **Detail → the `listing` RPC** (gives social proof + your save state):
  ```
  POST /rpc/listing { "id": "<uuid>" }
  → result: { listing: {...}, favorite_count: N, favorited: bool }
  ```
  Anon may call it; it returns active listings (the owner/admin also see their
  own non-active). Do **not** use `GET /api/listings/<id>` for public detail — it
  is owner-scoped and 401s anon / 404s a non-owner.
- **Create:** `POST /api/listings` (auth = user)
  ```
  { category_id, title, description?, price?, price_negotiable?, currency?,
    locale?, city_id?, district_id?, condition?,
    allow_chat?, allow_call?, allow_whatsapp?,        // channel flags (0/1)
    photos?: ["<media-id>", ...],                     // max 12
    attributes?: { <key>: <value>, ... } }            // per category_form
  → { row: {...} }
  ```
  `seller_id`, `id`, timestamps, and a 30-day `expires_at` are set server-side
  (never send `seller_id` — it's ignored). Invalid attributes → 400 with a
  reason ("missing required attribute: year", "invalid value for make", …).
- **Edit:** `PATCH /api/listings/<id>` (auth = owner) — partial; same validation.
  Send `attributes` (full object) to re-validate + re-index facets.
- **Delete:** `DELETE /api/listings/<id>` (auth = owner).
- **Status:** `draft | pending | active | sold | expired | removed`. Only
  `active` shows in search/browse. Mark sold via PATCH `{status:"sold"}`.

## Media

- **Upload:** `POST /media` with the **raw image bytes** as the body and the
  image `Content-Type` (e.g. `image/jpeg`). Auth required. ≤ 8 MiB.
  ```
  → 201 { id, width, height, variants: ["full", "thumb"] }
  ```
  The server validates + re-encodes to JPEG (rejects non-JPEG/PNG and oversized
  / decompression-bomb images → 400). **Pre-resize on the client** before upload
  (saves bandwidth; on mobile convert HEIC→JPEG via the native picker), but the
  server re-derives the canonical image regardless.
- **Serve:** `GET /media/<id>/<variant>` — `variant` ∈ `full` (≤1600px) |
  `thumb` (≤320px). Public, immutable-cached. Use `thumb` in grids, `full` on
  detail.
- **Flow:** upload all photos → collect `id`s → set `listings.photos` on
  create/edit.

## Search, browse & facets

One RPC powers text search, category/geo browse, filtering, sorting, and the
facet sidebar. Empty `q` = browse mode.

```
POST /rpc/search {
  q?,                        // free text (FTS; prefix + word, Cyrillic ok)
  category?, city?,          // ids
  filters?: [                // AND across entries
    { key: "make",  values: ["Toyota","Honda"] },   // enum/text: OR within
    { key: "year",  min: 2010, max: 2018 },          // numeric range (min/max optional)
    { key: "furnished", values: [1] }                // bool stored as 0/1
  ],
  sort?: "relevance" | "newest" | "price_asc" | "price_desc",
  limit?, offset?
}
→ result: {
    results: [ {id,title,price,currency,category_id,city_id,photos,created_at}, ... ],
    total, limit, offset,
    facets: { make: [ {value:"Toyota", count:12}, ... ], year: [...] }  // only when category set
  }
```

- `facets` are computed over the **base set** (q/category/city) *without* the
  user's filter selections, so the sidebar still shows the other refinements and
  their counts (classic drill-down). Render checkboxes/ranges from
  `category_form.attributes` (the `filterable` ones) + these counts.
- `results[].photos` here is the JSON-text array — parse, then build
  `GET /media/<id>/thumb` URLs.
- Default sort: relevance when `q` present, else newest.

## Contact (phone reveal)

- **Owner sets contact** (login, owner-only):
  `POST /rpc/set_listing_contact { listing_id, phone, whatsapp }`.
- **Reveal** (login-gated; logs an event; respects the channel flag):
  ```
  POST /rpc/reveal_contact { listing_id, channel?: "phone" | "whatsapp" }
  → result: { listing_id, phone? | whatsapp? }     // null result → 400
  ```
  Call on the user clicking "Show number" / "WhatsApp". For WhatsApp, build a
  `https://wa.me/<number>` deep link from the returned value. The reveal is the
  signal — don't pre-fetch it on page load.

## Chat (realtime)

A buyer↔seller conversation per listing. All login-gated; participants only.

- **Start / open:** `POST /rpc/start_conversation { listing_id }` →
  `{ conversation_id, existing }`. Idempotent (one per listing+buyer); blocked
  for your own listing and `allow_chat=0`.
- **Inbox:** `POST /rpc/inbox` →
  `{ conversations: [ {id, listing_id, listing_title, counterpart, last_message,
  last_message_at, buyer_id, seller_id}, ... ] }` (newest first).
- **Send:** `POST /api/message { conversation_id, body }` → `{row}`. `sender_id`
  is server-set; non-participants get 400.
- **History:** `GET /api/message?where={"conversation_id":{"eq":"<id>"}}&order=created_at`
  → `{count, rows}`. **Auto-scoped** to your conversations — a non-participant
  gets 0 rows even if they guess the id.
- **Live updates (WebSocket):** open a WS to the same origin (`/`), then over the
  binary opcode protocol:
  1. `LOGIN` (0x10) `{email, password}` → `{token}` (or pass an existing token).
  2. `SUBSCRIBE` (0x20) `{ token, table: "message",
     key: { column: "conversation_id", value: "<conv-id>" } }` → `{status:"ok"}`
     (membership-checked; non-participant → `{status:"error"}`).
  3. Receive `CHANGE` (0x22) frames `{table:"message", op:"INSERT", row:{...}}`
     as messages arrive. Subscribe **per open conversation**.

  Frame format (header `!BBHI` = opcode, flags, message_id, payload_len, then the
  JSON body) and a working raw client are in `tests/realtime_e2e.py` and
  `apps/classifieds/test_chat_ws.py` — copy that framing for the shared client.
  Inbox-level "new message in any chat" notifications are a later (push) feature.

## Favorites

- `POST /rpc/favorite   { listing_id }` → `{ favorited: true }`  (idempotent)
- `POST /rpc/unfavorite { listing_id }` → `{ favorited: false }`
- `POST /rpc/favorites` → `{ favorites: [ {id,title,price,...,status,saved_at}, ... ] }`
  (any status — a sold/expired save still shows, with its `status`).
- The `listing` RPC returns `favorited` (your state) + `favorite_count` (social
  proof) for the heart toggle + count.

## Trust & safety: report + moderation *(NEW — needs UI)*

Backend shipped (commit `f9d29c8`); the client work is a **report button** on the
listing detail + an **admin moderation queue** in the admin surface.

**User-facing — the report button** (`POST /rpc/report_listing`, role `user`, login-gated):
- Body `{ listing_id, reason, note? }`. `reason` ∈
  `spam | scam | prohibited | offensive | duplicate | miscat | other` (offer these as a small menu);
  `note` is free text (≤1000 chars, optional).
- Reply `{ result: { ok: true } }` on success. Validation → `{ ok: false, error }` (show inline).
- **Idempotent per user+listing** — a repeat report returns `ok:true` and changes nothing (no "already
  reported" error to handle). Reporting **your own** listing → `{ ok:false }`. Show a simple confirmation
  ("Thanks, our team will review"), not a report count.
- Only show the button to logged-in users (gate in UI; the server enforces it regardless).

**Admin — moderation queue** (role `admin`; all `POST /rpc/<fn>`):
- `list_reports { status?, limit?, offset? }` → `{ reports: [ { listing_id, title, listing_status,
  seller_id, reports (count), reasons (comma-str), last_reported }, … ] }`. Default `status:"open"` =
  the queue.
- `listing_reports { listing_id }` → `{ reports: [ { id, reporter_id, reason, note, status, created_at,
  resolved_at, resolved_by }, … ] }` — the drill-in for one listing.
- ⚠️ **Empty `reports` serializes as `{}`, not `[]`** (cellar's Lua→JSON quirk), and `{}` is **truthy**
  in JS — so `(res.result.reports || []).map(...)` throws on the (common) empty queue. **Normalize by
  shape, not truthiness:** `const rows = Array.isArray(res.result.reports) ? res.result.reports : []`.
  Applies to both `list_reports` and `listing_reports`.
- `takedown_listing { listing_id, note? }` → listing → `removed` (drops from public search/detail),
  its open reports → `actioned`, seller notified. `reinstate_listing { listing_id }` reverses it
  (→ `active`, fresh expiry). `dismiss_reports { listing_id }` = reviewed, no action (clears the queue row).
- Optional server auto-hide (`CLS_AUTO_HIDE_REPORTS` distinct reporters → listing `pending`, hidden but
  reversible) is **off by default**; if enabled, such listings show up with `listing_status:"pending"`.

## Security model the client should assume

- Treat the server as the source of truth: it enforces validation, ownership,
  membership, and gating. Client checks are UX only.
- Never expect to read another user's contact number, messages, or to edit their
  listings — those fail by design.
- The contact number and chat are intentionally **on-platform** (anti-scam +
  the moderation surface). Don't add an "export all numbers" affordance.
