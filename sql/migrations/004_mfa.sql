-- cellar 004: TOTP two-factor auth (#auth, PLAN §7e).
--
-- Opt-in (CEL_MFA) AND per-user: the second login step only triggers for a user
-- with a CONFIRMED enrollment, so an untouched deployment behaves exactly as
-- before. This only inserts a step between "password verified" and "session
-- issued"; every method still converges on the same opaque cel_sessions token.

-- A user's TOTP enrollment (at most one). confirmed_at NULL = setup not finished,
-- so it does NOT gate login yet (prevents locking someone out mid-enrollment).
CREATE TABLE IF NOT EXISTS cel_mfa (
    user_id      UUID PRIMARY KEY REFERENCES cel_users(id) ON DELETE CASCADE,
    type         TEXT NOT NULL DEFAULT 'totp',
    secret       TEXT NOT NULL,            -- base32 TOTP seed
    confirmed_at TIMESTAMPTZ,              -- NULL until the user proves one code
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Short-lived, single-use challenge issued after the password step (factor one
-- proven, factor two pending). Stored hashed at rest like sessions; attempts caps
-- code-guessing within the TTL window.
CREATE TABLE IF NOT EXISTS cel_mfa_challenges (
    token      TEXT PRIMARY KEY,          -- sha256(challenge)
    user_id    UUID NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
    attempts   INT NOT NULL DEFAULT 0,
    expires_at TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_cel_mfa_challenges_expires ON cel_mfa_challenges(expires_at);
