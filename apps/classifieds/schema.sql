-- ============================================================================
-- classifieds — catalog schema (A0.1–A0.3)
--
-- A single GLOBAL public catalog (server-of-record), not a per-device offline
-- app — so tables here carry NO rev/deleted sync columns (unlike clerkhalls).
-- Auth/users come from the engine (cel_users); this file adds the domain tables.
--
-- Design: docs/classifieds-design.md §6 (category/attribute model) + the
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
