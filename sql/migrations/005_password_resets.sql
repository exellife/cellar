-- pgforge 005: password-reset tokens (#auth, PLAN §7e).
--
-- Forgot-password emails a single-use token (stored hashed at rest, like sessions);
-- redeeming it sets a new password on the user's 'password' identity. Builds on the
-- mailer (libcurl SMTP). Inert unless the password-reset endpoints are used.
CREATE TABLE IF NOT EXISTS pgf_password_resets (
    token      TEXT PRIMARY KEY,        -- sha256(reset token); the raw token is emailed
    user_id    UUID NOT NULL REFERENCES pgf_users(id) ON DELETE CASCADE,
    expires_at TIMESTAMPTZ NOT NULL,
    used_at    TIMESTAMPTZ,             -- set when redeemed (single use)
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_pgf_password_resets_expires ON pgf_password_resets(expires_at);
