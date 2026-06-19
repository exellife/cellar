-- pgforge 003: demo of row-level ownership (multi-tenancy).
-- Each note belongs to the user in owner_id. With an owner_column policy, users
-- only see/modify their own rows; admin (superuser) sees all.

CREATE TABLE IF NOT EXISTS notes (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id   UUID NOT NULL,                -- the owning user (pgf_users.id)
    title      TEXT NOT NULL,
    body       TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_notes_owner ON notes(owner_id);
