/* ============================================================================
 * pgforge — outbound email (SMTP via libcurl).
 *
 * A thin transport: build an RFC 5322 message and hand it to libcurl's SMTP
 * support (smtp:// + STARTTLS or smtps://, with optional AUTH). No new dependency
 * — libcurl is already linked for OIDC JWKS. OPT-IN: inert unless PGF_SMTP_URL +
 * PGF_MAIL_FROM are set. Consumers (password reset, email verification) call
 * pgf_mail_send; they don't touch SMTP themselves.
 * ============================================================================ */
#ifndef PGF_MAILER_H
#define PGF_MAILER_H

#include <stdbool.h>

/* Read SMTP config from the environment (call once at startup, after
 * curl_global_init):
 *   PGF_SMTP_URL       smtp://host:587 or smtps://host:465   (required to enable)
 *   PGF_SMTP_USER/_PASS  AUTH credentials                    (optional)
 *   PGF_SMTP_TLS       require (default) | try | none        (STARTTLS policy)
 *   PGF_MAIL_FROM      envelope + header From address        (required to enable)
 *   PGF_MAIL_FROM_NAME display name                          (optional)            */
void pgf_mailer_init(void);

/* True when a usable SMTP config is present (URL + From). */
bool pgf_mail_enabled(void);

/* Send a plain-text/UTF-8 email to a single recipient. Returns 0 on success, -1
 * on failure or when disabled. `to`/`subject` are rejected if they contain CR/LF
 * (header-injection guard). */
int pgf_mail_send(const char *to, const char *subject, const char *body);

#endif /* PGF_MAILER_H */
