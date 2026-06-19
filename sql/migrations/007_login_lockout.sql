-- cellar 007: per-account login lockout (#auth, PLAN §7e).
--
-- Tracks consecutive failed password logins per account so the login path can
-- lock an account after N failures within a window (CEL_AUTH_LOCKOUT="N/W"). This
-- is a per-ACCOUNT defense that complements the per-IP rate limiter. Opt-in: the
-- columns are inert unless CEL_AUTH_LOCKOUT is set. Federated logins are unaffected
-- (lockout lives on the password path).
ALTER TABLE cel_users ADD COLUMN IF NOT EXISTS failed_login_count   INTEGER NOT NULL DEFAULT 0;
ALTER TABLE cel_users ADD COLUMN IF NOT EXISTS last_failed_login_at TIMESTAMPTZ;
ALTER TABLE cel_users ADD COLUMN IF NOT EXISTS locked_until         TIMESTAMPTZ;
