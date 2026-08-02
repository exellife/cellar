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

/* Optional per-recipient outbound throttle (anti mail-bomb). main.c creates the
 * limiter and installs it here; NULL/unset disables it (all mail allowed). The key
 * is a canonicalized recipient (lowercased, +tag stripped, gmail dots removed) so
 * trivial aliases of one inbox share a bucket. */
typedef struct cel_ratelimit cel_ratelimit_t;
void cel_mail_set_ratelimit(cel_ratelimit_t *rl);
/* A SEPARATE per-recipient limiter for NOTIFICATION email, so high app-notification
 * volume can't exhaust the small auth-mail budget and starve verification/reset
 * delivery (audit re-verify). Independent bucket, same canonical-recipient key. */
void cel_mail_set_notif_ratelimit(cel_ratelimit_t *rl);

/* True if another email may be sent to `to` right now (canonicalized + rate-checked);
 * true when the throttle is disabled. `_recipient_allowed` gates the auth-mail paths;
 * `_notif_allowed` gates notification mail against its own bucket. */
bool cel_mail_recipient_allowed(const char *to);
bool cel_mail_notif_allowed(const char *to);

/* Send a plain-text/UTF-8 email to a single recipient using the process-wide From
 * (CEL_MAIL_FROM / _FROM_NAME). Returns 0 on success, -1 on failure or when
 * disabled. `to`/`subject` are rejected if they contain CR/LF (header-injection). */
int cel_mail_send(const char *to, const char *subject, const char *body);

/* Same, but with a per-app From override: `from_addr`/`from_name` (from the bundle's
 * `_mail`) replace the process-wide From for both the header and the SMTP envelope;
 * NULL/empty falls back to the global. SMTP credentials + server stay process-wide
 * (secrets never come from a bundle). From is also CR/LF-guarded. */
int cel_mail_send_from(const char *to, const char *subject, const char *body,
                       const char *from_addr, const char *from_name);

/* Full send: optional HTML alongside the plain text. When `html_body` is non-empty
 * the message is `multipart/alternative` (text part = `text_body`, html part =
 * `html_body`) so non-HTML clients still get the text; when NULL/empty it's plain
 * text/plain. `text_body` is always required. From override + fallback as above.
 * The bodies are NOT CR/LF-guarded (they are the MIME payload, after the headers). */
int cel_mail_send_html(const char *to, const char *subject,
                       const char *text_body, const char *html_body,
                       const char *from_addr, const char *from_name);

#endif /* CEL_MAILER_H */
