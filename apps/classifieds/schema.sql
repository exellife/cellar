-- ============================================================================
-- classifieds — catalog schema (A0.1–A0.3)
--
-- A single GLOBAL public catalog (server-of-record), not a per-device offline
-- app — so tables here carry NO rev/deleted sync columns (unlike clerkhalls).
-- Auth/users come from the engine (cel_users); this file adds the domain tables.
--
-- Design: classifieds-design.md §6 (category/attribute model) + the
-- "bake in now" rules (global UUID ids, region tag, currency+locale).
--
-- Two layers (do not conflate):
--   Layer 1 — taxonomy & attributes as DATA (category, category_attribute):
--             admins add a category + its fields with no migration / no deploy.
--   Layer 2 — listings store category-specific values as JSON, with a derived
--             typed listing_facet index for fast faceted filtering.
--
-- IDs are app-generated UUIDs (TEXT) everywhere — never autoincrement: it
-- collides the moment we shard/merge regions (the subtle one people regret).
-- ============================================================================

PRAGMA foreign_keys = ON;

-- ── Layer 1: taxonomy as data ───────────────────────────────────────────────

-- The category tree (self-referential). `slug` is the URL key; `name` is the
-- default display label, `labels` an optional {"ru":…, "ky":…, "en":…} override.
CREATE TABLE category (
  id         TEXT PRIMARY KEY,
  parent_id  TEXT REFERENCES category(id) ON DELETE CASCADE,
  slug       TEXT NOT NULL UNIQUE,
  name       TEXT NOT NULL,
  labels     TEXT,                              -- JSON: localized names (nullable)
  icon       TEXT,
  sort_order INTEGER NOT NULL DEFAULT 0,
  is_active  INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL DEFAULT '',
  updated_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_category_parent ON category (parent_id, sort_order);

-- Per-category attribute schema. This ONE table generates the post form, the
-- filter/facet UI, and write-time validation — no per-category code.
--   type        : enum | int | number | bool | text
--   filterable  : 1 => this attr is materialized into listing_facet (a facet)
--   options     : JSON array of allowed values (for type=enum)
--   depends_on  : key of a parent attr in the same category (e.g. model→make)
CREATE TABLE category_attribute (
  id         TEXT PRIMARY KEY,
  category_id TEXT NOT NULL REFERENCES category(id) ON DELETE CASCADE,
  key        TEXT NOT NULL,
  label      TEXT NOT NULL,
  labels     TEXT,                              -- JSON: localized labels (nullable)
  type       TEXT NOT NULL CHECK (type IN ('enum','int','number','bool','text')),
  required   INTEGER NOT NULL DEFAULT 0,
  filterable INTEGER NOT NULL DEFAULT 0,
  unit       TEXT,                              -- e.g. 'km', 'm²'
  options    TEXT,                              -- JSON array (for type=enum)
  depends_on TEXT,                              -- key of a parent attr (make→model)
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT '',
  updated_at TEXT NOT NULL DEFAULT '',
  UNIQUE (category_id, key)
);

-- ── Geo reference tables (oblast → city → district) ─────────────────────────
-- Inherently regional (one catalog = one country). city_id/district_id are
-- first-class FKs on listings; big taxonomies are reference tables, not enums.

CREATE TABLE geo_oblast (
  id         TEXT PRIMARY KEY,
  name       TEXT NOT NULL,
  labels     TEXT,                              -- JSON: localized (nullable)
  sort_order INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE geo_city (
  id         TEXT PRIMARY KEY,
  oblast_id  TEXT NOT NULL REFERENCES geo_oblast(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,
  labels     TEXT,
  sort_order INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_geo_city_oblast ON geo_city (oblast_id, sort_order);

CREATE TABLE geo_district (
  id         TEXT PRIMARY KEY,
  city_id    TEXT NOT NULL REFERENCES geo_city(id) ON DELETE CASCADE,
  name       TEXT NOT NULL,
  labels     TEXT,
  sort_order INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_geo_district_city ON geo_district (city_id, sort_order);

-- ── Layer 2: listings + derived facet index ─────────────────────────────────

-- Common fields are first-class indexed columns (filtered/sorted on every
-- query); category-specific values live in `attributes` (JSON). Money is stored
-- as a whole-unit INTEGER in `currency` (classifieds prices are whole numbers;
-- no float). `region` is the shard/country tag baked in now. `locale` is the
-- language of title/description.
CREATE TABLE listings (
  -- id is minted server-side by SQLite (uuid4), the gen_random_uuid() analogue —
  -- clients never supply it (no trusted ids on a public catalog).
  id          TEXT PRIMARY KEY DEFAULT (
                lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
                substr(lower(hex(randomblob(2))),2)||'-'||
                substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||
                '-'||lower(hex(randomblob(6)))),
  region      TEXT NOT NULL DEFAULT 'kg',
  seller_id   TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
  category_id TEXT NOT NULL REFERENCES category(id),
  title       TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  price       INTEGER,                          -- whole units of `currency`; NULL = unpriced
  price_negotiable INTEGER NOT NULL DEFAULT 0,  -- "договорная"
  currency    TEXT NOT NULL DEFAULT 'KGS',
  locale      TEXT NOT NULL DEFAULT 'ru',
  city_id     TEXT REFERENCES geo_city(id),
  district_id TEXT REFERENCES geo_district(id),
  condition   TEXT,                             -- e.g. new | used (category-refined)
  status      TEXT NOT NULL DEFAULT 'active'
              CHECK (status IN ('draft','pending','active','sold','expired','removed')),
  -- The status a listing held before an admin/auto MODERATION hold (takedown ->
  -- 'removed', auto-hide -> 'pending'), so reinstate can restore the true prior
  -- state instead of guessing 'active' (which would resurrect a sold/expired item).
  -- '' when the listing is not under a hold. Empty for every publicly-visible row.
  pre_moderation_status TEXT NOT NULL DEFAULT '',
  -- contact CHANNEL flags are public (the detail page shows the right buttons);
  -- the number itself lives in listing_contact, never exposed on this row.
  allow_chat     INTEGER NOT NULL DEFAULT 1,
  allow_call     INTEGER NOT NULL DEFAULT 1,
  allow_whatsapp INTEGER NOT NULL DEFAULT 0,
  photos      TEXT NOT NULL DEFAULT '[]',       -- JSON: ordered media ids (POST /media)
  attributes  TEXT NOT NULL DEFAULT '{}',       -- JSON: category-specific values
  created_at  TEXT NOT NULL DEFAULT '',
  updated_at  TEXT NOT NULL DEFAULT '',
  expires_at  TEXT
);
CREATE INDEX idx_listings_cat_status   ON listings (category_id, status, created_at);
CREATE INDEX idx_listings_city_status  ON listings (city_id, status, created_at);
CREATE INDEX idx_listings_seller       ON listings (seller_id, status);
CREATE INDEX idx_listings_expiry       ON listings (status, expires_at);  -- expiry sweep (Phase 2)

-- Derived "typed EAV, but only for filterable attrs": one row per filterable
-- attribute per listing, populated by the facet-sync hook (A0.5) from the JSON.
-- A numeric attr uses num_value (range queries); enum/text uses text_value.
-- Derived index, rebuilt idempotently by the facet-sync hook (A0.5, after()).
-- ON DELETE CASCADE cleans facets when a listing is removed.
CREATE TABLE listing_facet (
  listing_id TEXT NOT NULL REFERENCES listings(id) ON DELETE CASCADE,
  key        TEXT NOT NULL,
  num_value  REAL,
  text_value TEXT,
  PRIMARY KEY (listing_id, key)
);
CREATE INDEX idx_facet_key_num  ON listing_facet (key, num_value);
CREATE INDEX idx_facet_key_text ON listing_facet (key, text_value);

-- ── Full-text search (A1.3) ─────────────────────────────────────────────────
-- An external-content FTS5 index over listings' title + description, kept in
-- sync by triggers (the canonical FTS5 external-content pattern). Maps to
-- listings by rowid; the search rpc joins fts.rowid → listings.rowid.
--
-- Tokenizer: unicode61 with Unicode case-folding + diacritic removal — handles
-- ru/ky (Cyrillic case folding) and gives word + prefix ("term*") search with
-- bm25 ranking. (A trigram index for true mid-word substring matching is a
-- later enhancement; unicode61 + prefix covers the common partial-word case.)
CREATE VIRTUAL TABLE listings_fts USING fts5(
  title, description,
  content='listings', content_rowid='rowid',
  tokenize='unicode61 remove_diacritics 2'
);

CREATE TRIGGER listings_fts_ai AFTER INSERT ON listings BEGIN
  INSERT INTO listings_fts(rowid, title, description)
    VALUES (new.rowid, new.title, new.description);
END;
CREATE TRIGGER listings_fts_ad AFTER DELETE ON listings BEGIN
  INSERT INTO listings_fts(listings_fts, rowid, title, description)
    VALUES ('delete', old.rowid, old.title, old.description);
END;
CREATE TRIGGER listings_fts_au AFTER UPDATE ON listings BEGIN
  INSERT INTO listings_fts(listings_fts, rowid, title, description)
    VALUES ('delete', old.rowid, old.title, old.description);
  INSERT INTO listings_fts(rowid, title, description)
    VALUES (new.rowid, new.title, new.description);
END;

-- ── Contact (A1.6) ──────────────────────────────────────────────────────────
-- The actual contact details — DELIBERATELY a separate table, never a column on
-- listings, so an anon /api/listings read can't leak the number. Readable only
-- by the gated reveal_contact rpc (server-side) and the owner; policy denies
-- direct anon/user access.
CREATE TABLE listing_contact (
  listing_id TEXT PRIMARY KEY REFERENCES listings(id) ON DELETE CASCADE,
  phone      TEXT,
  whatsapp   TEXT,
  updated_at TEXT NOT NULL DEFAULT ''
);

-- Contact events — the gated, identity-bearing demand signal (every reveal/chat/
-- whatsapp click is a logged-in user acting on a listing). The richer generic
-- EventSink + browsing telemetry (views/clicks) is Phase 2; this is the
-- contact-specific log that powers "responsive seller" badges + demand metrics.
CREATE TABLE contact_event (
  id         TEXT PRIMARY KEY DEFAULT (
               lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
               substr(lower(hex(randomblob(2))),2)||'-'||
               substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||
               '-'||lower(hex(randomblob(6)))),
  listing_id TEXT NOT NULL REFERENCES listings(id) ON DELETE CASCADE,
  actor_id   TEXT NOT NULL,                      -- the (logged-in) viewer
  seller_id  TEXT NOT NULL,                      -- the listing owner
  kind       TEXT NOT NULL
             CHECK (kind IN ('number_revealed','whatsapp_clicked','chat_started')),
  created_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_contact_event_listing ON contact_event (listing_id, created_at);
CREATE INDEX idx_contact_event_seller  ON contact_event (seller_id, created_at);

-- ── Chat (A1.6 part 2) ──────────────────────────────────────────────────────
-- A buyer↔seller conversation about a listing (one per listing+buyer).
CREATE TABLE conversation (
  id         TEXT PRIMARY KEY DEFAULT (
               lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
               substr(lower(hex(randomblob(2))),2)||'-'||
               substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||
               '-'||lower(hex(randomblob(6)))),
  listing_id TEXT NOT NULL REFERENCES listings(id) ON DELETE CASCADE,
  buyer_id   TEXT NOT NULL,
  seller_id  TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT '',
  last_message_at TEXT NOT NULL DEFAULT '',
  UNIQUE (listing_id, buyer_id)
);
CREATE INDEX idx_conversation_buyer  ON conversation (buyer_id, last_message_at);
CREATE INDEX idx_conversation_seller ON conversation (seller_id, last_message_at);

-- Membership table for the realtime VIA rule + read scoping: one row per
-- participant. The engine's owner_via scope (message list/get) and the realtime
-- subscribe membership check both query this — so a user can only read/subscribe
-- to messages of conversations they belong to (no /api or WS leak).
CREATE TABLE conversation_member (
  conversation_id TEXT NOT NULL REFERENCES conversation(id) ON DELETE CASCADE,
  user_id         TEXT NOT NULL,
  PRIMARY KEY (conversation_id, user_id)
);

CREATE TABLE message (
  id         TEXT PRIMARY KEY DEFAULT (
               lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
               substr(lower(hex(randomblob(2))),2)||'-'||
               substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||
               '-'||lower(hex(randomblob(6)))),
  conversation_id TEXT NOT NULL REFERENCES conversation(id) ON DELETE CASCADE,
  sender_id  TEXT NOT NULL,
  body       TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_message_conv ON message (conversation_id, created_at);

-- ── Favorites (A1.7) ────────────────────────────────────────────────────────
-- A user's saved listings (composite key = the toggle). Accessed via rpcs only.
CREATE TABLE favorite (
  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
  listing_id TEXT NOT NULL REFERENCES listings(id) ON DELETE CASCADE,
  created_at TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (user_id, listing_id)
);
CREATE INDEX idx_favorite_listing ON favorite (listing_id);   -- favorite_count

-- ── Notifications (in-app feed, A2.2) ───────────────────────────────────────
-- A per-user feed (the bell): persisted source of truth, realtime-enabled so an
-- online user gets live pushes (owner-scoped, like chat) and an offline user
-- sees them on next load. Created server-side (hooks/jobs via the notify helper).
CREATE TABLE notification (
  id         TEXT PRIMARY KEY DEFAULT (
               lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
               substr(lower(hex(randomblob(2))),2)||'-'||
               substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||
               '-'||lower(hex(randomblob(6)))),
  user_id    TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
  type       TEXT NOT NULL,                  -- message | listing_expired | ...
  title      TEXT NOT NULL DEFAULT '',
  body       TEXT NOT NULL DEFAULT '',
  subject_id TEXT,                            -- related entity (listing/conversation) for deep-link
  data       TEXT,                            -- optional JSON
  read_at    TEXT,                            -- NULL = unread
  created_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_notification_user   ON notification (user_id, created_at);
CREATE INDEX idx_notification_unread ON notification (user_id, read_at);

-- ── Saved searches + alerts (A2.3) ──────────────────────────────────────────
-- A user's saved query (q + category + city). A recurring matcher job finds
-- listings created since last_run_at that match, and notifies the user.
CREATE TABLE saved_search (
  id          TEXT PRIMARY KEY DEFAULT (
                lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||
                substr(lower(hex(randomblob(2))),2)||'-'||
                substr('89ab',abs(random())%4+1,1)||substr(lower(hex(randomblob(2))),2)||
                '-'||lower(hex(randomblob(6)))),
  user_id     TEXT NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
  name        TEXT NOT NULL DEFAULT '',
  q           TEXT NOT NULL DEFAULT '',
  category_id TEXT,
  city_id     TEXT,
  notify      INTEGER NOT NULL DEFAULT 1,
  created_at  TEXT NOT NULL DEFAULT '',
  last_run_at TEXT NOT NULL DEFAULT ''     -- cursor: only match listings created after this
);
CREATE INDEX idx_saved_search_user ON saved_search (user_id);

-- App-owned display profile for a cel_users row (the engine's cel_users has only
-- email/role — no display name). id = cel_users.id. Set via the set_profile rpc;
-- read by search (seller_name) + the listing seller block. display_name is NULL
-- until the user sets one (the client shows a placeholder), so no email is exposed.
CREATE TABLE user_profile (
  id           TEXT PRIMARY KEY REFERENCES cel_users(id) ON DELETE CASCADE,   -- = cel_users.id
  display_name TEXT,
  created_at   TEXT NOT NULL DEFAULT '',
  updated_at   TEXT NOT NULL DEFAULT ''
);

-- ── Trust & safety: listing reports (moderation queue) ──────────────────────
-- Users flag a listing (login-gated report_listing rpc); admins review the queue
-- (list_reports) and act (takedown_listing / reinstate_listing / dismiss_reports).
-- ONE open report per (listing, reporter) — the UNIQUE index makes a repeat a
-- no-op (INSERT OR IGNORE), so a single account can't inflate a listing's report
-- count (the anti-brigading property the optional auto-hide relies on). Reports
-- are never exposed via /api (admin-only there); all access is through the rpcs.
CREATE TABLE listing_report (
  id           TEXT PRIMARY KEY,
  listing_id   TEXT NOT NULL REFERENCES listings(id) ON DELETE CASCADE,
  reporter_id  TEXT,                                    -- cel_users.id (rpc is login-gated)
  reason       TEXT NOT NULL
               CHECK (reason IN ('spam','scam','prohibited','offensive','duplicate','miscat','other')),
  note         TEXT NOT NULL DEFAULT '',
  status       TEXT NOT NULL DEFAULT 'open'
               CHECK (status IN ('open','actioned','dismissed')),
  created_at   TEXT NOT NULL DEFAULT '',
  resolved_at  TEXT NOT NULL DEFAULT '',
  resolved_by  TEXT                                     -- admin cel_users.id that resolved it
);
-- Dedupe is scoped to OPEN reports (partial index): one open report per
-- (listing, reporter) — a repeat while open is a no-op (INSERT OR IGNORE) — but
-- once a report is resolved (actioned/dismissed) the same reporter CAN file a
-- fresh 'open' report if the listing reoffends (e.g. seller edits abuse back in).
CREATE UNIQUE INDEX idx_report_dedupe  ON listing_report (listing_id, reporter_id) WHERE status = 'open';
CREATE INDEX        idx_report_queue   ON listing_report (status, created_at);
CREATE INDEX        idx_report_listing ON listing_report (listing_id);
