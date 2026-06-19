-- cellar 008: TOTP recovery codes (#auth, PLAN §7e).
--
-- One-time backup codes issued when a user confirms TOTP enrollment (and on
-- regenerate). Stored hashed at rest; a code can be used in place of a TOTP code
-- at the second login step, then it's consumed. This is self-service 2FA lockout
-- recovery (lost authenticator), so admin `mfa-reset` is the fallback, not the norm.
CREATE TABLE IF NOT EXISTS cel_mfa_recovery (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id    UUID NOT NULL REFERENCES cel_users(id) ON DELETE CASCADE,
    code_hash  TEXT NOT NULL,            -- sha256 of the normalized recovery code
    used_at    TIMESTAMPTZ,             -- set when redeemed (single use)
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_cel_mfa_recovery_user ON cel_mfa_recovery(user_id);
