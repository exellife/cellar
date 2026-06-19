-- cellar 001: core auth schema (users + opaque server-side sessions)
-- Generic, app-agnostic. Product tables (products, categories, …) come later.

CREATE EXTENSION IF NOT EXISTS pgcrypto;   -- gen_random_uuid()

-- Application users (dashboard operators / admins).
CREATE TABLE IF NOT EXISTS cel_users (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email         TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,                 -- libsodium crypto_pwhash_str (argon2id)
    role          TEXT NOT NULL DEFAULT 'viewer',-- admin | editor | viewer
    is_active     BOOLEAN NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ
);

-- Opaque session tokens: verification = a lookup here. Revoke = delete the row.
CREATE TABLE IF NOT EXISTS cel_sessions (
    token       TEXT PRIMARY KEY,                -- 256-bit random, hex-encoded
    user_id     UUID NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at  TIMESTAMPTZ NOT NULL,
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_cel_sessions_user    ON cel_sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_cel_sessions_expires ON cel_sessions(expires_at);
