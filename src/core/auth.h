/* cellar — authentication (opaque server-side sessions over Postgres). */
#ifndef CEL_AUTH_H
#define CEL_AUTH_H

#include <stddef.h>
#include <stdbool.h>

#define CEL_AUTH_OK       0
#define CEL_AUTH_INVALID (-1)   /* bad credentials / unknown or expired token */
#define CEL_AUTH_DBERR   (-2)   /* database/connection failure */
#define CEL_AUTH_CONFLICT (-3)  /* registration: email already exists */
#define CEL_AUTH_LOCKED  (-4)   /* login: account temporarily locked (too many failures) */
#define CEL_AUTH_MFA_REQUIRED 1 /* login: password ok, second factor needed (challenge issued) */

typedef struct {
    char id[37];          /* user UUID (text) */
    char email[256];
    char role[32];        /* admin | editor | viewer | platform_admin */
    bool email_verified;  /* email_verified_at IS NOT NULL */
} cel_user_t;

/* Precompute the login decoy hash (timing equalization for unknown users; call
 * once at startup, after cel_crypto_init). MUST succeed: returns 0 on success, -1
 * if the Argon2id precompute failed — the caller should refuse to start, since
 * without it login latency reveals which emails exist (L-1). */
int cel_auth_init(void);

/* Configure per-account login lockout: after `limit` failed password attempts
 * within `window_seconds`, the account is locked for `window_seconds`. limit <= 0
 * (or a non-positive window) disables lockout entirely (the default). */
void cel_auth_set_lockout(int limit, int window_seconds);

/* Clear a lockout / reset the failure count for an account (admin recovery).
 * Returns the number of users affected (0 or 1), or -1 on error. */
int cel_auth_unlock(const char *email);

/* Verify credentials and, unless a second factor is required, create a session.
 * On CEL_AUTH_OK `out_token` holds the opaque session token. If the user has a
 * confirmed TOTP enrollment (and MFA is enabled), no session is issued: returns
 * CEL_AUTH_MFA_REQUIRED with a short-lived challenge in `out_challenge` (the
 * caller completes login via cel_mfa_verify_login). `out_user` is filled in both
 * cases. Both buffers must be >= 65 bytes. Returns CEL_AUTH_*. */
int cel_auth_login(const char *email, const char *password,
                   char *out_token, size_t token_size,
                   char *out_challenge, size_t challenge_size,
                   cel_user_t *out_user);

/* Mint a session for an ALREADY-authenticated user: insert a hashed-token row and
 * return the raw token (>= 65 bytes), refreshing last_login_at. Shared by the
 * password path, MFA verification, and federated login. The session lifetime +
 * renewal come from the active per-app session policy (cel_session_set_active),
 * not a caller arg. Returns CEL_AUTH_*. */
int cel_auth_issue_session(const char *user_id, char *out_token, size_t token_size);

/* Federated sign-in (the DB half of OAuth — the ID token is already verified):
 * resolve a (provider, sub) identity to a session. An existing identity logs in;
 * else, IF `email_link_trusted` (the provider is authoritative for this email's
 * domain), a verified email matching an existing account links to it — an
 * untrusted collision is REFUSED (CEL_AUTH_INVALID), never silently merged (H-3);
 * else, if `provision_role` is non-empty AND an email is present, auto-provision a
 * new user with that role. Honors MFA: if the resolved user has a confirmed TOTP
 * enrollment, returns CEL_AUTH_MFA_REQUIRED with a challenge in `out_challenge`
 * (the caller completes via cel_mfa_verify_login) instead of minting a session
 * (H-2). On success mints a session (out_token >= 65) and fills out_user.
 * Returns CEL_AUTH_OK / MFA_REQUIRED / INVALID (no/again untrusted account) / DBERR. */
int cel_auth_oauth_login(const char *provider, const char *sub,
                         const char *email, bool email_verified, bool email_link_trusted,
                         const char *provision_role,
                         char *out_token, size_t token_size,
                         char *out_challenge, size_t challenge_size, cel_user_t *out_user);

/* Self-service registration: create a user with `role`, then create a session
 * (auto-login) and return its opaque token + the user. Returns CEL_AUTH_CONFLICT
 * if the email is already taken. `out_token` must be >= 65 bytes. The caller is
 * responsible for deciding whether `role` is permitted for signup (see
 * cel_role_can_self_register). */
int cel_auth_register(const char *email, const char *password, const char *role,
                      char *out_token, size_t token_size,
                      cel_user_t *out_user);

/* Admin-provisioned account creation: insert a user with `role`. No session is
 * created — the new user logs in themselves. Writes the new user's id (text) into
 * `out_id` (>= 37 bytes). Returns CEL_AUTH_CONFLICT for a duplicate email.
 * Authorization (who may create which role) is the caller's responsibility — see
 * cel_api_create_user. */
int cel_auth_create_user(const char *email, const char *password, const char *role,
                         char *out_id, size_t out_id_size);

/* Resolve a token to its user (if the session is valid and unexpired). */
int cel_auth_verify(const char *token, cel_user_t *out_user);

/* Like cel_auth_verify but consults the in-memory session cache first (when
 * enabled): a hit returns immediately without a DB round-trip; a miss falls back
 * to cel_auth_verify and populates the cache. This is what request handlers use. */
int cel_auth_resolve(const char *token, cel_user_t *out_user);

/* Revoke a session token. Returns CEL_AUTH_OK even if it was already gone. */
int cel_auth_logout(const char *token);

/* Create a single-use password-reset token for the 'password' account with this
 * email. On success writes the raw token (>= 65 bytes) for the caller to email and
 * returns CEL_AUTH_OK. Returns CEL_AUTH_INVALID when no such password account
 * exists — the caller still responds 200 (don't reveal whether the email is known). */
int cel_auth_create_password_reset(const char *email, char *out_token, size_t token_size);

/* Redeem a reset token: set a new password on the user's 'password' identity, mark
 * the token used (single use), and revoke all the user's sessions. Returns
 * CEL_AUTH_OK / CEL_AUTH_INVALID (bad/expired/used token) / DBERR. The caller
 * validates password length first. */
int cel_auth_perform_password_reset(const char *token, const char *new_password);

/* In-session "change password": verify `current_password` against the user's
 * 'password' identity, then set `new_password`. Keyed by `user_id` (the
 * authenticated caller). Sessions are left intact (the caller stays signed in).
 * Returns CEL_AUTH_OK, CEL_AUTH_INVALID (wrong current password / no password
 * identity), or CEL_AUTH_DBERR. The caller validates new-password length first. */
int cel_auth_change_password(const char *user_id, const char *current_password,
                             const char *new_password);

/* Privileged "set password" (admin reset, no email round-trip): set `new_password`
 * on the 'password' identity for `email` WITHOUT knowing the current one — unlike
 * change_password, there is no current-password check, so the caller MUST authorize
 * it (this backs cellar.set_password, whose bundle rpc enforces who-may-reset-whom).
 * Keyed by `email` (the password identity's provider_uid). As a full takeover
 * recovery it revokes the target's sessions + pending MFA + clears lockout. Returns
 * CEL_AUTH_OK, CEL_AUTH_INVALID (no 'password' account for that email), or
 * CEL_AUTH_DBERR. The caller validates new-password length first. */
int cel_auth_set_password(const char *email, const char *new_password);

/* Read the role of the 'password' account for `email` (caller-side authorization
 * input — e.g. to refuse resetting a superuser). Writes the role into `out_role`.
 * Returns CEL_AUTH_OK, CEL_AUTH_INVALID (no such password account), or
 * CEL_AUTH_DBERR. */
int cel_auth_user_role(const char *email, char *out_role, size_t out_role_size);

/* ---- device tokens (PIN fast-login) --------------------------------------- *
 * A long-lived, revocable credential a client stores (encrypted, e.g. behind a
 * PIN) and exchanges for fresh sessions without re-entering the password. Opt-in
 * per app via _session.device_ttl_seconds; the token is hashed at rest. */

/* Mint a device token for `user_id`. Writes the raw token (>= 65) + the public id
 * (>= 37, for list/revoke). `ttl_seconds` is the app's device_ttl_seconds. Returns
 * CEL_AUTH_OK / CEL_AUTH_INVALID (bad args) / CEL_AUTH_DBERR. */
int cel_auth_device_create(const char *user_id, const char *label, int ttl_seconds,
                           char *out_token, size_t token_size, char *out_id, size_t id_size);

/* Exchange a device token for a fresh session via the active session strategy:
 * validates unexpired + not-revoked + active user, bumps last_used_at, issues the
 * session into `out_token` (>= 65) and fills `out_user`. Returns CEL_AUTH_OK /
 * CEL_AUTH_INVALID (bad/expired/revoked) / CEL_AUTH_DBERR. Does NOT require MFA —
 * the enrolled device is the possession factor. */
int cel_auth_device_exchange(const char *device_token, char *out_token, size_t token_size,
                             cel_user_t *out_user);

/* Revoke one of `user_id`'s OWN device tokens by public id. CEL_AUTH_OK /
 * CEL_AUTH_INVALID (no such active device for this user) / CEL_AUTH_DBERR. */
int cel_auth_device_revoke(const char *user_id, const char *id);

/* List a user's active (unexpired, unrevoked) device tokens; `cb` fires once per
 * row (never the token value). CEL_AUTH_OK / CEL_AUTH_DBERR. */
typedef void (*cel_device_cb)(void *ctx, const char *id, const char *label,
                              long created_at, long last_used_at, long expires_at);
int cel_auth_device_list(const char *user_id, cel_device_cb cb, void *ctx);

/* ---- web-push subscriptions (NotifChannel off-site delivery) --------------- *
 * One row per browser/device a user enabled push on. The engine owns these
 * (like sessions/device-tokens); the NotifChannel fan-out resolves them. */

/* Register a push subscription for `user_id`, upserting by `endpoint` (re-subscribe
 * refreshes keys + re-enables). Writes the public id (>=37). OK / INVALID / DBERR. */
int cel_auth_push_subscribe(const char *user_id, const char *endpoint,
                            const char *p256dh, const char *auth, const char *ua,
                            char *out_id, size_t out_id_size);

/* Remove one of `user_id`'s subscriptions, matched by endpoint OR public id.
 * CEL_AUTH_OK / CEL_AUTH_INVALID (none matched) / CEL_AUTH_DBERR. */
int cel_auth_push_unsubscribe(const char *user_id, const char *endpoint_or_id);

/* List a user's active (not-disabled) subscriptions; `cb` fires once per row
 * (no key material). CEL_AUTH_OK / CEL_AUTH_DBERR. */
typedef void (*cel_push_cb)(void *ctx, const char *id, const char *endpoint,
                            const char *ua, long created_at, long last_used_at);
int cel_auth_push_list(const char *user_id, cel_push_cb cb, void *ctx);

/* Generate a fresh 6-digit email-verification CODE for `user_id`. On success
 * writes the plaintext code (>= 7 bytes) + the account email (for the caller to
 * send to) and stores sha256(code) with a short TTL, resetting the attempt count
 * (one pending code per user). Returns CEL_AUTH_OK, CEL_AUTH_CONFLICT if the email
 * is already verified (caller sends nothing), CEL_AUTH_LOCKED if a code was sent
 * within the resend cooldown, CEL_AUTH_INVALID if the user is unknown. */
int cel_auth_create_email_code(const char *user_id,
                               char *out_code, size_t code_size,
                               char *out_email, size_t email_size);

/* Verify a 6-digit code against the AUTHENTICATED `user_id`. On a match, marks
 * the account email verified and consumes the code. A wrong code increments a
 * capped attempt counter; wrong/expired/too-many/none all return CEL_AUTH_INVALID
 * (the client requests a fresh code). CEL_AUTH_OK on success, DBERR on failure. */
int cel_auth_verify_email_code(const char *user_id, const char *code);

/* Revoke ALL sessions for the user with this email (e.g. on password change,
 * "log out everywhere", or suspend) and clear the session cache. Returns the
 * number of sessions deleted, or -1 on error. */
int cel_auth_revoke_user_sessions(const char *email);

/* Upsert a user with a given role (first-run seeding). Returns 0 on success. */
int cel_auth_seed_user(const char *email, const char *password, const char *role);
/* Convenience: cel_auth_seed_user(email, password, "admin"). */
int cel_auth_seed_admin(const char *email, const char *password);

#endif /* CEL_AUTH_H */
