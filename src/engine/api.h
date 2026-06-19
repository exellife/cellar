/* ============================================================================
 * pgforge — transport-neutral engine API.
 *
 * The data/auth operations as plain functions over JSON, independent of how the
 * request arrived. Both the WebSocket opcode handlers and the REST routes are
 * thin adapters over these: they parse a request into a cJSON, call here, then
 * serialize the returned body for their transport. `http_status` doubles as the
 * success/error signal (>= 400 means error) for both.
 * ============================================================================ */
#ifndef PGF_API_H
#define PGF_API_H

#include <cjson/cJSON.h>
#include "policy.h"
#include "realtime.h"

typedef struct {
    cJSON *body;        /* response object (caller owns / cJSON_Delete) */
    int    http_status; /* 200 ok; >=400 error (401/403/404/400/500/...) */
} pgf_api_result_t;

/* All data ops take the caller identity and are authorization-checked (deny by
 * default): unauthenticated -> 401, insufficient role -> 403.
 *   list  : { table, select?, where?, order?, limit?, offset? }
 *   get   : { table, id }                                                   */
pgf_api_result_t pgf_api_list(const pgf_identity_t *who, const cJSON *req);
pgf_api_result_t pgf_api_get(const pgf_identity_t *who, const cJSON *req);
pgf_api_result_t pgf_api_schema(const pgf_identity_t *who);

/* login is public (no identity required). With TOTP 2FA enabled, a user who has a
 * confirmed enrollment gets 200 + {status:"mfa_required", challenge} instead of a
 * token; the client completes login by POSTing the challenge + code to mfa_verify. */
pgf_api_result_t pgf_api_login(const cJSON *req);

/* TOTP two-factor (PLAN §7e). enroll/confirm/disable act on the authenticated
 * caller's own account; verify is public (it's the second login step, gated by the
 * challenge + code, not a session).
 *   enroll  : {}                       -> 200 {secret, otpauth_uri} / 403 (off) / 409 (already)
 *   confirm : { code }                 -> 200 {status:"confirmed"} / 401 / 400
 *   disable : { code }                 -> 200 / 401 / 400
 *   verify  : { challenge, code }      -> 200 {token, user} / 401                          */
pgf_api_result_t pgf_api_mfa_enroll(const pgf_identity_t *who);
pgf_api_result_t pgf_api_mfa_confirm(const pgf_identity_t *who, const cJSON *req);
pgf_api_result_t pgf_api_mfa_disable(const pgf_identity_t *who, const cJSON *req);
pgf_api_result_t pgf_api_mfa_verify(const cJSON *req);
/* Regenerate one-time recovery codes (needs a current TOTP code). confirm also
 * returns the initial set. verify accepts a recovery code in place of a TOTP code.
 *   recovery-codes : { code }  -> 200 {recovery_codes:[...]} / 401                 */
pgf_api_result_t pgf_api_mfa_recovery(const pgf_identity_t *who, const cJSON *req);

/* Federated sign-in (OIDC, "Option B"): the client passes an ID token it obtained
 * from the provider; pgforge verifies it and find-or-links the identity.
 *   oauth : { provider, id_token }  -> 200 {token, user} / 401 (bad token) / 403   */
pgf_api_result_t pgf_api_oauth(const cJSON *req);

/* Password reset (emailed, single-use token; builds on the mailer). forgot always
 * returns 200 (anti-enumeration); reset redeems the token, sets the new password,
 * and revokes the user's sessions.
 *   forgot : { email }            -> 200 (always)
 *   reset  : { token, password }  -> 200 / 400 (bad/expired token, weak password)  */
pgf_api_result_t pgf_api_password_forgot(const cJSON *req);
pgf_api_result_t pgf_api_password_reset(const cJSON *req);

/* Email verification (emailed single-use token; sent on register). verify redeems
 * the token; resend re-sends to the authenticated caller (idempotent).
 *   verify-email        : { token }  -> 200 / 400
 *   verify-email/resend : (Bearer)   -> 200 (always)                              */
pgf_api_result_t pgf_api_verify_email(const cJSON *req);
pgf_api_result_t pgf_api_verify_email_resend(const pgf_identity_t *who);

/* register is public self-service signup. Gated by the role config: only roles
 * flagged self-registerable (and never superusers) can be created this way; see
 * pgf_role_can_self_register. On success auto-logs-in (returns a session token).
 *   register : { email, password, role? }  -> 201 + {token, user} / 403 / 409   */
pgf_api_result_t pgf_api_register(const cJSON *req);

/* Admin-provisioned account creation (authenticated, superuser-only). Cannot
 * create platform_admin (out-of-band/CLI only — keeps the platform tier
 * unreachable in-band). In pooled mode a tenant admin's new users are forced
 * into the admin's own tenant; platform_admin must pass an explicit tenant_id.
 *   create user : { email, password, role, tenant_id? } -> 201 + {user} / 403 / 409 */
pgf_api_result_t pgf_api_create_user(const pgf_identity_t *who, const cJSON *req);

/*   create : { table, values: {col: v, ...} }       -> 201 + inserted row
 *   update : { table, id, values: {col: v, ...} }    -> 200 + updated row / 404
 *   delete : { table, id }                            -> 200 + deleted row / 404  */
pgf_api_result_t pgf_api_create(const pgf_identity_t *who, const cJSON *req);
pgf_api_result_t pgf_api_update(const pgf_identity_t *who, const cJSON *req);
pgf_api_result_t pgf_api_delete(const pgf_identity_t *who, const cJSON *req);

/* Call a whitelisted Postgres function as an authz'd operation (domain logic as
 * SQL). Deny-by-default via the "_rpc" config; args are bound, not interpolated;
 * runs under the caller's tenant context (RLS) like any write.
 *   rpc : { fn, args?: {name: value, ...} }  -> 200 + { result: [rows] } / 403 / 400 */
pgf_api_result_t pgf_api_rpc(const pgf_identity_t *who, const cJSON *req);

/* Startup audit: warn about whitelisted RPC functions defined SECURITY DEFINER,
 * which bypass row-level security (a tenant-isolation risk in pooled mode). Call
 * once after the policy is loaded and the DB is available (H-4). */
void pgf_rpc_audit_security_definer(void);

/* Authorize a realtime subscription request { table, key?:{column,value} } for
 * `who`, reusing the LIST access rules: the table must be readable by the role
 * and realtime-enabled; the caller's scope becomes the delivery predicates in
 * *sub. A VIA-scoped (membership) table requires a `key` naming its local column;
 * membership is verified by one query and collapses to an equality predicate.
 * Returns an HTTP status (200 ok) and, on failure, a reason in errbuf. */
int pgf_api_authorize_subscription(const pgf_identity_t *who, const cJSON *req,
                                   pgf_subscription_t *sub, char *errbuf, size_t errlen);

/* Publish-time re-authorization for a VIA (membership) subscription: re-verifies
 * the subscriber is still a member (M-5). Register with pgf_realtime_init. */
bool pgf_api_rt_recheck_member(const pgf_subscription_t *sub);

#endif /* PGF_API_H */
