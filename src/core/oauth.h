/* ============================================================================
 * pgforge — OIDC sign-in (Google / Apple / generic), the verify half.
 *
 * "Option B" (PLAN §7e): the client obtains an ID token from the provider and
 * POSTs it here; pgforge verifies the token's RS256 signature against the
 * provider's published JWKS (fetched + cached via libcurl), checks iss/aud/exp,
 * and returns the verified subject + email. The DB half (find-or-link an identity
 * and mint a session) is pgf_auth_oauth_login in auth.c. We never run the
 * interactive OAuth redirect/code dance — only token verification.
 * ============================================================================ */
#ifndef PGF_OAUTH_H
#define PGF_OAUTH_H

#include <stdbool.h>
#include <stddef.h>

/* Verified claims from an OIDC ID token. */
typedef struct {
    char sub[256];        /* the provider's stable subject id (-> identity provider_uid) */
    char email[256];      /* may be empty */
    bool email_verified;
} pgf_oauth_claims_t;

/* Configure providers from the environment: PGF_OAUTH_PROVIDERS=name[,name...] and
 * per-provider PGF_OAUTH_<NAME>_CLIENT_ID (audience), _ISSUER, _JWKS. "google" and
 * "apple" supply built-in issuer + JWKS, so only the client id is required. Call
 * once at startup (after curl_global_init). */
void pgf_oauth_init(void);
void pgf_oauth_cleanup(void);

/* True if at least one provider is configured. */
bool pgf_oauth_enabled(void);

/* Verify an ID token for `provider`: RS256 signature (>= 2048-bit key) against the
 * provider's JWKS, plus iss / aud / azp / exp / iat. If `expected_nonce` is non-NULL
 * and non-empty, the token's nonce claim must match it (front-channel replay
 * defense; pass NULL to skip). Fills `out` and returns 0 on success; -1 on any
 * failure, with a short reason in `errbuf` (log it, don't return it — see L-2). */
int pgf_oauth_verify(const char *provider, const char *id_token, const char *expected_nonce,
                     pgf_oauth_claims_t *out, char *errbuf, size_t errlen);

/* True iff `provider` may auto-link a federated identity to an existing local
 * account with this `email` — i.e. the email's domain is in the provider's
 * PGF_OAUTH_<NAME>_TRUSTED_DOMAINS allow-list. Fails closed (no allow-list =>
 * never link). Prevents cross-provider account takeover by email (H-3). */
bool pgf_oauth_email_link_allowed(const char *provider, const char *email);

#endif /* PGF_OAUTH_H */
