-- Session tokens are now stored hashed at rest (SHA-256): the cel_sessions.token
-- column holds the hash, not the raw bearer token. Existing rows hold raw tokens
-- (which the new hash-and-compare verify can no longer match, and which are the
-- plaintext credentials we're hardening away), so clear them — everyone re-logs in.
DELETE FROM cel_sessions;
