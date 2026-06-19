-- cellar 006: email verification (#auth, PLAN §7e).
--
-- Records whether an account's email has been confirmed, and holds the emailed,
-- single-use verification tokens (hashed at rest, like password resets). A
-- verification email is sent on registration; redeeming the token sets
-- email_verified_at. Federated logins arrive already verified.
ALTER TABLE cel_users ADD COLUMN IF NOT EXISTS email_verified_at TIMESTAMPTZ;

CREATE TABLE IF NOT EXISTS cel_email_verifications (
    token      TEXT PRIMARY KEY,        -- sha256(token); the raw token is emailed
    user_id    UUID NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL,
    used_at    TIMESTAMPTZ,             -- set when redeemed (single use)
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_cel_email_verifications_expires ON cel_email_verifications(expires_at);
