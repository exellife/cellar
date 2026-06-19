/* ============================================================================
 * cellar — outbound email (SMTP via libcurl).
 *
 * A thin transport: build an RFC 5322 message and hand it to libcurl's SMTP
 * support (smtp:// + STARTTLS or smtps://, with optional AUTH). No new dependency
 * — libcurl is already linked for OIDC JWKS. OPT-IN: inert unless CEL_SMTP_URL +
 * CEL_MAIL_FROM are set. Consumers (password reset, email verification) call
 * cel_mail_send; they don't touch SMTP themselves.
 * ============================================================================ */
#ifndef CEL_MAILER_H
#define CEL_MAILER_H

#include <stdbool.h>

/* Read SMTP config from the environment (call once at startup, after
 * curl_global_init):
 *   CEL_SMTP_URL       smtp://host:587 or smtps://host:465   (required to enable)
 *   CEL_SMTP_USER/_PASS  AUTH credentials                    (optional)
 *   CEL_SMTP_TLS       require (default) | try | none        (STARTTLS policy)
 *   CEL_MAIL_FROM      envelope + header From address        (required to enable)
 *   CEL_MAIL_FROM_NAME display name                          (optional)            */
void cel_mailer_init(void);

/* True when a usable SMTP config is present (URL + From). */
bool cel_mail_enabled(void);

/* Send a plain-text/UTF-8 email to a single recipient. Returns 0 on success, -1
 * on failure or when disabled. `to`/`subject` are rejected if they contain CR/LF
 * (header-injection guard). */
int cel_mail_send(const char *to, const char *subject, const char *body);

#endif /* CEL_MAILER_H */
