-- pgforge 003: split credentials out of pgf_users into pgf_identities.
--
-- One account (pgf_users) can have many login methods (password, google, apple,
-- ...), each a row here. The password credential moves out of pgf_users.password_hash
-- into a provider='password' identity keyed by the user's email; the column is then
-- dropped so the credential lives in exactly one place. Federated login (#auth,
-- PLAN §7e) and 2FA build on this. Every method still converges on the same opaque
-- pgf_sessions token, so authz/RLS/session-cache are unchanged.

CREATE TABLE IF NOT EXISTS pgf_identities (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id      UUID NOT NULL REFERENCES pgf_users(id) ON DELETE CASCADE,
    provider     TEXT NOT NULL,          -- 'password' | 'google' | 'apple' | ...
    provider_uid TEXT NOT NULL,          -- email (password); OIDC 'sub' (federated)
    secret       TEXT,                   -- argon2id hash (password); NULL for OAuth
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (provider, provider_uid)
);

CREATE INDEX IF NOT EXISTS idx_pgf_identities_user ON pgf_identities(user_id);

-- Backfill + drop, guarded so it runs once and is safe on a fresh DB (where 001
-- just created password_hash) and on re-runs (where it's already gone). The whole
-- migration is wrapped in a transaction by the runner, so backfill-then-drop is
-- atomic: either every password becomes an identity and the column goes, or nothing.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name   = 'pgf_users'
                 AND column_name  = 'password_hash') THEN
        INSERT INTO pgf_identities (user_id, provider, provider_uid, secret)
        SELECT id, 'password', email, password_hash FROM pgf_users
        ON CONFLICT (provider, provider_uid) DO NOTHING;

        ALTER TABLE pgf_users DROP COLUMN password_hash;
    END IF;
END $$;
