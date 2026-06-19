-- cellar 003: split credentials out of cel_users into cel_identities.
--
-- One account (cel_users) can have many login methods (password, google, apple,
-- ...), each a row here. The password credential moves out of cel_users.password_hash
-- into a provider='password' identity keyed by the user's email; the column is then
-- dropped so the credential lives in exactly one place. Federated login (#auth,
-- PLAN §7e) and 2FA build on this. Every method still converges on the same opaque
-- cel_sessions token, so authz/RLS/session-cache are unchanged.

CREATE TABLE IF NOT EXISTS cel_identities (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id      UUID NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
    provider     TEXT NOT NULL,          -- 'password' | 'google' | 'apple' | ...
    provider_uid TEXT NOT NULL,          -- email (password); OIDC 'sub' (federated)
    secret       TEXT,                   -- argon2id hash (password); NULL for OAuth
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (provider, provider_uid)
);

CREATE INDEX IF NOT EXISTS idx_cel_identities_user ON cel_identities(user_id);

-- Backfill + drop, guarded so it runs once and is safe on a fresh DB (where 001
-- just created password_hash) and on re-runs (where it's already gone). The whole
-- migration is wrapped in a transaction by the runner, so backfill-then-drop is
-- atomic: either every password becomes an identity and the column goes, or nothing.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.columns
               WHERE table_schema = 'public'
                 AND table_name   = 'cel_users'
                 AND column_name  = 'password_hash') THEN
        INSERT INTO cel_identities (user_id, provider, provider_uid, secret)
        SELECT id, 'password', email, password_hash FROM cel_users
        ON CONFLICT (provider, provider_uid) DO NOTHING;

        ALTER TABLE cel_users DROP COLUMN password_hash;
    END IF;
END $$;
