-- ClerkHalls — event-hall booking management, on cellar.
-- One bundle = one ORGANIZATION (file-isolated). All tables are SYNCABLE (rev +
-- deleted) so the whole dataset syncs across the org's staff devices, offline-first.
-- Hierarchy:  organization → venues → halls (bookable spaces) → bookings
--             → {booking_item_categories, booking_items, payments};  menu catalog
--             (menu_categories → menu_items) feeds booking_items of kind 'menu'.
-- ON DELETE CASCADE is honored under sync (engine cascades the soft-delete).

-- org-level settings (a single row, id='org')
CREATE TABLE organization (
  id         TEXT PRIMARY KEY DEFAULT 'org',
  name       TEXT NOT NULL DEFAULT '',
  currency   TEXT NOT NULL DEFAULT 'KGS',
  timezone   TEXT,
  phone      TEXT,
  address    TEXT,
  created_at TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT '',
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0
);

-- a venue / location (a business can run several)
CREATE TABLE venues (
  id         TEXT PRIMARY KEY,
  name       TEXT NOT NULL,
  address    TEXT, phone TEXT, notes TEXT,
  is_active  INTEGER NOT NULL DEFAULT 1,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0
);

-- a bookable hall (event space, ~30–500 capacity) inside a venue
CREATE TABLE halls (
  id         TEXT PRIMARY KEY,
  venue_id   TEXT NOT NULL,
  name       TEXT NOT NULL,
  capacity   INTEGER, notes TEXT,
  is_active  INTEGER NOT NULL DEFAULT 1,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (venue_id) REFERENCES venues(id) ON DELETE CASCADE
);
CREATE INDEX idx_halls_venue ON halls (venue_id, sort_order);

-- an event in one room, for one session (the conflict unit: hall_id+session+date)
CREATE TABLE bookings (
  id TEXT PRIMARY KEY,
  hall_id TEXT NOT NULL,
  session TEXT NOT NULL,                 -- morning | afternoon | evening
  start_date TEXT NOT NULL, end_date TEXT NOT NULL, start_time TEXT,
  customer_name TEXT NOT NULL, customer_phone TEXT,
  event_type TEXT NOT NULL,              -- wedding | beshik_toi | ... (localized)
  guest_count_min INTEGER NOT NULL DEFAULT 0, guest_count_max INTEGER NOT NULL DEFAULT 0,
  price_per_person REAL NOT NULL DEFAULT 0,
  discount REAL NOT NULL DEFAULT 0, deposit REAL NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'tentative',  -- tentative | confirmed | completed | cancelled
  notes TEXT,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (hall_id) REFERENCES halls(id)   -- NO cascade: deleting a room must not wipe its bookings
);
CREATE INDEX idx_bookings_hall_session_date ON bookings (hall_id, session, start_date);
CREATE INDEX idx_bookings_start_date ON bookings (start_date);

-- per-booking grouping for itemized lines
CREATE TABLE booking_item_categories (
  id TEXT PRIMARY KEY, booking_id TEXT NOT NULL, kind TEXT NOT NULL, name TEXT NOT NULL,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (booking_id) REFERENCES bookings(id) ON DELETE CASCADE
);

-- unified itemized lines (kind ∈ menu, extra, wholesale, byo)
CREATE TABLE booking_items (
  id TEXT PRIMARY KEY, booking_id TEXT NOT NULL, kind TEXT NOT NULL,
  catalog_item_id TEXT, category_id TEXT, name TEXT NOT NULL,
  quantity TEXT NOT NULL DEFAULT '0', unit_price REAL, remaining TEXT, notes TEXT,
  sort_order INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (booking_id) REFERENCES bookings(id) ON DELETE CASCADE,
  FOREIGN KEY (catalog_item_id) REFERENCES menu_items(id) ON DELETE SET NULL
);
CREATE INDEX idx_booking_items_booking_kind ON booking_items (booking_id, kind, sort_order);

-- staged, multi-currency, multi-method payments
CREATE TABLE payments (
  id TEXT PRIMARY KEY, booking_id TEXT NOT NULL, paid_at TEXT NOT NULL,
  stage TEXT NOT NULL, method TEXT NOT NULL, currency TEXT NOT NULL DEFAULT 'KGS',
  amount REAL NOT NULL DEFAULT 0, amount_kgs REAL NOT NULL DEFAULT 0,
  exchange_rate REAL, source TEXT, notes TEXT,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (booking_id) REFERENCES bookings(id) ON DELETE CASCADE
);
CREATE INDEX idx_payments_booking ON payments (booking_id, paid_at);

-- menu catalog (optional pre-curation; booking_items kind='menu' may reference an item)
CREATE TABLE menu_categories (
  id TEXT PRIMARY KEY, name TEXT NOT NULL,
  sort_order INTEGER NOT NULL DEFAULT 0,
  is_active INTEGER NOT NULL DEFAULT 1, is_default INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE menu_items (
  id TEXT PRIMARY KEY, category_id TEXT NOT NULL, name TEXT NOT NULL,
  price_kgs REAL, description TEXT,
  sort_order INTEGER NOT NULL DEFAULT 0, is_active INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
  rev INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (category_id) REFERENCES menu_categories(id) ON DELETE CASCADE
);
CREATE INDEX idx_menu_items_cat ON menu_items (category_id, sort_order);
