/* ============================================================================
 * cellar — TOTP two-factor auth: DB-backed enrollment + verification.
 *
 * Sits between the password step and session issuance. It is OPT-IN (CEL_MFA)
 * and per-user: the second login step only triggers for a user with a CONFIRMED
 * enrollment, so a deployment that never enables it — or never enrolls anyone —
 * behaves exactly as before. The crypto is in totp.{c,h}; this owns cel_mfa and
 * cel_mfa_challenges and the session it ultimately mints (via cel_auth_issue_session).
 * ============================================================================ */
#ifndef CEL_MFA_H
#define CEL_MFA_H

#include "auth.h"   /* cel_user_t */
#include <stdbool.h>
#include <stddef.h>

#define CEL_MFA_OK            0
#define CEL_MFA_DBERR       (-1)
#define CEL_MFA_INVALID     (-2)   /* bad code or bad/expired challenge */
#define CEL_MFA_NOT_ENROLLED (-3)
#define CEL_MFA_ALREADY     (-4)   /* enroll: already confirmed */
#define CEL_MFA_DISABLED    (-5)   /* feature is off for this deployment */

/* Deployment mode (default OFF — fully inert). Set once at startup. */
#define CEL_MFA_MODE_OFF      0
#define CEL_MFA_MODE_OPTIONAL 1
void cel_mfa_set_mode(int mode);
int  cel_mfa_mode(void);

/* True if this user must complete the second step now: MFA enabled AND a
 * confirmed enrollment exists. Drives the branch in cel_auth_login. */
bool cel_mfa_required_for(const char *user_id);

/* Begin enrollment: generate a fresh secret (replacing any UNCONFIRMED one) and
 * return it plus an otpauth:// URI (for the QR; labelled with the account email,
 * looked up from user_id). Refuses if already confirmed (CEL_MFA_ALREADY) or the
 * feature is off (CEL_MFA_DISABLED). */
int cel_mfa_enroll(const char *user_id,
                   char *out_secret, size_t secret_size,
                   char *out_uri, size_t uri_size);

/* How many one-time recovery codes are issued, and the per-code buffer size. */
#define CEL_MFA_RECOVERY_N   10
#define CEL_MFA_RECOVERY_LEN 48   /* "xxxxxxxx-xxxxxxxx-xxxxxxxx-xxxxxxxx" + NUL (128-bit code) */

/* Finish enrollment: the user proves one current code; sets confirmed_at and
 * issues a fresh set of recovery codes into `out_codes` (CEL_MFA_RECOVERY_N
 * entries) — shown to the user ONCE. */
int cel_mfa_confirm(const char *user_id, const char *code,
                    char out_codes[][CEL_MFA_RECOVERY_LEN]);

/* Regenerate the recovery codes (invalidating the old set). Requires a valid
 * current TOTP code, like disable. Fills `out_codes`. */
int cel_mfa_regenerate_recovery(const char *user_id, const char *code,
                                char out_codes[][CEL_MFA_RECOVERY_LEN]);

/* Turn MFA off for the user — requires a valid current code (so a hijacked
 * session can't silently strip 2FA). Removes the enrollment and any challenges. */
int cel_mfa_disable(const char *user_id, const char *code);

/* Issue a single-use, short-lived login challenge for a user who passed factor
 * one. Writes the raw challenge token (>= 65 bytes) to `out_challenge`. */
int cel_mfa_create_challenge(const char *user_id, char *out_challenge, size_t size);

/* Second login step: validate the challenge + TOTP code, then mint a session.
 * Fills `out_token` (>= 65 bytes) and `out_user` on success. */
int cel_mfa_verify_login(const char *challenge, const char *code,
                         char *out_token, size_t token_size, cel_user_t *out_user);

/* Admin lockout recovery: remove a user's enrollment by email (e.g. the
 * `cellar mfa-reset` CLI). Returns rows removed, or -1 on error. */
int cel_mfa_reset(const char *email);

#endif /* CEL_MFA_H */
