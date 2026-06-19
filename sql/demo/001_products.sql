-- cellar 002: demo product catalog (for engine development / introspection).
-- Representative of a real admin dashboard: a parent table + a child with an FK,
-- a mix of column types, defaults, nullability.

CREATE TABLE IF NOT EXISTS categories (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name        TEXT NOT NULL UNIQUE,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS products (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    category_id UUID REFERENCES categories(id) ON DELETE SET NULL,
    name        TEXT NOT NULL,
    sku         TEXT UNIQUE,
    price       NUMERIC(12,2) NOT NULL DEFAULT 0,
    in_stock    INTEGER NOT NULL DEFAULT 0,
    is_active   BOOLEAN NOT NULL DEFAULT TRUE,
    metadata    JSONB,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_products_category ON products(category_id);

-- Seed a little data (idempotent on the unique keys).
INSERT INTO categories (name, description) VALUES
    ('Beverages', 'Drinks and liquids'),
    ('Snacks', 'Quick bites')
ON CONFLICT (name) DO NOTHING;

INSERT INTO products (category_id, name, sku, price, in_stock)
SELECT c.id, v.name, v.sku, v.price, v.stock
FROM (VALUES
    ('Beverages', 'Cola 330ml',   'BVG-001', 1.20, 240),
    ('Beverages', 'Water 500ml',  'BVG-002', 0.80, 500),
    ('Snacks',    'Potato Chips', 'SNK-001', 2.50, 120)
) AS v(cat, name, sku, price, stock)
JOIN categories c ON c.name = v.cat
ON CONFLICT (sku) DO NOTHING;
