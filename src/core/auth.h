/* pgforge — authentication (opaque server-side sessions over Postgres). */
#ifndef PGF_AUTH_H
#define PGF_AUTH_H

#include <stddef.h>
#include <stdbool.h>

#define PGF_AUTH_OK       0
#define PGF_AUTH_INVALID (-1)   /* bad credentials / unknown or expired token */
#define PGF_AUTH_DBERR   (-2)   /* database/connection failure */
#define PGF_AUTH_CONFLICT (-3)  /* registration: email already exists */
#define PGF_AUTH_LOCKED  (-4)   /* login: account temporarily locked (too many failures) */
#define PGF_AUTH_MFA_REQUIRED 1 /* login: password ok, second factor needed (challenge issued) */

typedef struct {
    char id[37];          /* user UUID (text) */
    char email[256];
    char role[32];        /* admin | editor | viewer | platform_admin */
    char tenant_id[37];   /* tenant UUID (text); "" in single-tenant deployments */
    bool email_verified;  /* email_verified_at IS NOT NULL */
} pgf_user_t;

/* Precompute the decoy password hash (call once at startup, after pgf_crypto_init). */
void pgf_auth_init(void);

/* Configure per-account login lockout: after `limit` failed password attempts
 * within `window_seconds`, the account is locked for `window_seconds`. limit <= 0
 * (or a non-positive window) disables lockout entirely (the default). */
void pgf_auth_set_lockout(int limit, int window_seconds);

/* Clear a lockout / reset the failure count for an account (admin recovery).
 * Returns the number of users affected (0 or 1), or -1 on error. */
int pgf_auth_unlock(const char *email);

/* Verify credentials and, unless a second factor is required, create a session.
 * On PGF_AUTH_OK `out_token` holds the opaque session token. If the user has a
 * confirmed TOTP enrollment (and MFA is enabled), no session is issued: returns
 * PGF_AUTH_MFA_REQUIRED with a short-lived challenge in `out_challenge` (the
 * caller completes login via pgf_mfa_verify_login). `out_user` is filled in both
 * cases. Both buffers must be >= 65 bytes. Returns PGF_AUTH_*. */
int pgf_auth_login(const char *email, const char *password, int ttl_seconds,
                   char *out_token, size_t token_size,
                   char *out_challenge, size_t challenge_size,
                   pgf_user_t *out_user);

/* Mint a session for an ALREADY-authenticated user: insert a hashed-token row and
 * return the raw token (>= 65 bytes), refreshing last_login_at. Shared by the
 * password path, MFA verification, and federated login. Returns PGF_AUTH_*. */
int pgf_auth_issue_session(const char *user_id, int ttl_seconds,
                           char *out_token, size_t token_size);

/* Federated sign-in (the DB half of OAuth — the ID token is already verified):
 * resolve a (provider, sub) identity to a session. An existing identity logs in;
 * else, IF `email_link_trusted` (the provider is authoritative for this email's
 * domain), a verified email matching an existing account links to it — an
 * untrusted collision is REFUSED (PGF_AUTH_INVALID), never silently merged (H-3);
 * else, if `provision_role` is non-empty AND an email is present, auto-provision a
 * new user with that role. Honors MFA: if the resolved user has a confirmed TOTP
 * enrollment, returns PGF_AUTH_MFA_REQUIRED with a challenge in `out_challenge`
 * (the caller completes via pgf_mfa_verify_login) instead of minting a session
 * (H-2). On success mints a session (out_token >= 65) and fills out_user.
 * Returns PGF_AUTH_OK / MFA_REQUIRED / INVALID (no/again untrusted account) / DBERR. */
int pgf_auth_oauth_login(const char *provider, const char *sub,
                         const char *email, bool email_verified, bool email_link_trusted,
                         const char *provision_role, int ttl_seconds,
                         char *out_token, size_t token_size,
                         char *out_challenge, size_t challenge_size, pgf_user_t *out_user);

/* Self-service registration: create a user with `role`, then create a session
 * (auto-login) and return its opaque token + the user. Returns PGF_AUTH_CONFLICT
 * if the email is already taken. `out_token` must be >= 65 bytes. The caller is
 * responsible for deciding whether `role` is permitted for signup (see
 * pgf_role_can_self_register). */
int pgf_auth_register(const char *email, const char *password, const char *role,
                      int ttl_seconds, char *out_token, size_t token_size,
                      pgf_user_t *out_user);

/* Admin-provisioned account creation: insert a user with `role` and (when
 * `tenant_id` is non-NULL/non-empty) bind it to that tenant. No session is
 * created — the new user logs in themselves. Writes the new user's id (text)
 * into `out_id` (>= 37 bytes). Returns PGF_AUTH_CONFLICT for a duplicate email,
 * PGF_AUTH_INVALID for an invalid tenant reference. Authorization (who may
 * create which role/tenant) is the caller's responsibility — see pgf_api_create_user. */
int pgf_auth_create_user(const char *email, const char *password, const char *role,
                         const char *tenant_id, char *out_id, size_t out_id_size);

/* Resolve a token to its user (if the session is valid and unexpired). */
int pgf_auth_verify(const char *token, pgf_user_t *out_user);

/* Like pgf_auth_verify but consults the in-memory session cache first (when
 * enabled): a hit returns immediately without a DB round-trip; a miss falls back
 * to pgf_auth_verify and populates the cache. This is what request handlers use. */
int pgf_auth_resolve(const char *token, pgf_user_t *out_user);

/* Revoke a session token. Returns PGF_AUTH_OK even if it was already gone. */
int pgf_auth_logout(const char *token);

/* Create a single-use password-reset token for the 'password' account with this
 * email. On success writes the raw token (>= 65 bytes) for the caller to email and
 * returns PGF_AUTH_OK. Returns PGF_AUTH_INVALID when no such password account
 * exists — the caller still responds 200 (don't reveal whether the email is known). */
int pgf_auth_create_password_reset(const char *email, char *out_token, size_t token_size);

/* Redeem a reset token: set a new password on the user's 'password' identity, mark
 * the token used (single use), and revoke all the user's sessions. Returns
 * PGF_AUTH_OK / PGF_AUTH_INVALID (bad/expired/used token) / DBERR. The caller
 * validates password length first. */
int pgf_auth_perform_password_reset(const char *token, const char *new_password);

/* Create a single-use email-verification token for `user_id`. On success writes
 * the raw token (>= 65 bytes) and the account email (for the caller to send to),
 * returning PGF_AUTH_OK. Returns PGF_AUTH_CONFLICT if the email is already
 * verified (caller sends nothing), PGF_AUTH_INVALID if the user is unknown. */
int pgf_auth_create_email_verification(const char *user_id,
                                       char *out_token, size_t token_size,
                                       char *out_email, size_t email_size);

/* Redeem a verification token: mark the account's email verified (single use).
 * Returns PGF_AUTH_OK / PGF_AUTH_INVALID (bad/expired/used) / DBERR. */
int pgf_auth_verify_email(const char *token);

/* Revoke ALL sessions for the user with this email (e.g. on password change,
 * "log out everywhere", or suspend) and clear the session cache. Returns the
 * number of sessions deleted, or -1 on error. */
int pgf_auth_revoke_user_sessions(const char *email);

/* Upsert a user with a given role (first-run seeding). Returns 0 on success. */
int pgf_auth_seed_user(const char *email, const char *password, const char *role);
/* Convenience: pgf_auth_seed_user(email, password, "admin"). */
int pgf_auth_seed_admin(const char *email, const char *password);

#endif /* PGF_AUTH_H */
