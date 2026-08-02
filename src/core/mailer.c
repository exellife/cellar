#include "mailer.h"
#include "password.h"   /* cel_random_token_hex (Message-ID) */
#include "rate_limit.h"
#include "logger.h"

#include <curl/curl.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TLS_NONE = 0, TLS_TRY = 1, TLS_REQUIRE = 2 };

static char g_url[256];
static char g_user[128];
static char g_pass[256];
static char g_from[256];
static char g_from_name[128];
static int  g_tls = TLS_REQUIRE;

/* Optional per-recipient outbound throttle (set by main.c). Bounds how many
 * verification/reset emails any one mailbox can be sent — an anti-abuse guard so a
 * scripted register/resend loop can't mail-bomb a victim through our sending domain. */
static cel_ratelimit_t *g_recipient_rl = NULL;   /* auth mail: verification / password reset */
static cel_ratelimit_t *g_notif_rl     = NULL;   /* notifications — a SEPARATE bucket so app
                                                  * notification volume can't starve the small
                                                  * auth-mail budget (audit re-verify) */

void cel_mail_set_ratelimit(cel_ratelimit_t *rl)       { g_recipient_rl = rl; }
void cel_mail_set_notif_ratelimit(cel_ratelimit_t *rl) { g_notif_rl = rl; }

/* Canonicalize a recipient for throttling so trivial aliases collapse to one key:
 * lowercase; drop a "+tag" from the local part; for gmail/googlemail also drop dots
 * (they alias to the same inbox) and fold to gmail.com. Unknown domains → lowercase. */
static void canon_recipient(const char *to, char *out, size_t cap) {
    const char *at = strchr(to, '@');
    if (!at || at == to || !at[1]) {           /* no usable @ → just lowercase */
        size_t i = 0; for (; to[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)to[i]);
        out[i] = 0; return;
    }
    char local[160] = {0}, domain[160] = {0};
    size_t ll = (size_t)(at - to); if (ll >= sizeof local) ll = sizeof local - 1;
    for (size_t i = 0; i < ll; i++) local[i] = (char)tolower((unsigned char)to[i]);
    for (size_t i = 0; at[1 + i] && i + 1 < sizeof domain; i++) domain[i] = (char)tolower((unsigned char)at[1 + i]);
    char *plus = strchr(local, '+'); if (plus) *plus = 0;   /* strip +tag */
    if (strcmp(domain, "gmail.com") == 0 || strcmp(domain, "googlemail.com") == 0) {
        char *r = local, *w = local; for (; *r; r++) if (*r != '.') *w++ = *r; *w = 0;
        snprintf(out, cap, "%s@gmail.com", local);
    } else {
        snprintf(out, cap, "%s@%s", local, domain);
    }
}

static bool rl_allow(cel_ratelimit_t *rl, const char *to) {
    if (!rl || !to || !*to) return true;               /* disabled → allow */
    char key[340]; canon_recipient(to, key, sizeof key);
    return cel_ratelimit_allow(rl, key);
}

bool cel_mail_recipient_allowed(const char *to) { return rl_allow(g_recipient_rl, to); }
bool cel_mail_notif_allowed(const char *to)     { return rl_allow(g_notif_rl, to); }

static void setenv_str(char *dst, size_t cap, const char *name) {
    const char *v = getenv(name);
    if (v && *v) snprintf(dst, cap, "%s", v);
}

/* Redact the userinfo (user:pass@) from an SMTP URL for logging — libcurl honors
 * credentials embedded in the URL (smtp://user:pass@host), so logging it verbatim
 * would leak the SMTP password to stdout/journald/log files (audit). */
static void redact_url(const char *url, char *out, size_t cap) {
    const char *scheme = strstr(url, "://");
    const char *at = scheme ? strchr(scheme + 3, '@') : NULL;
    if (scheme && at) snprintf(out, cap, "%.*s***@%s", (int)(scheme + 3 - url), url, at + 1);
    else              snprintf(out, cap, "%s", url);
}

void cel_mailer_init(void) {
    setenv_str(g_url,       sizeof g_url,       "CEL_SMTP_URL");
    setenv_str(g_user,      sizeof g_user,      "CEL_SMTP_USER");
    setenv_str(g_pass,      sizeof g_pass,      "CEL_SMTP_PASS");
    setenv_str(g_from,      sizeof g_from,      "CEL_MAIL_FROM");
    setenv_str(g_from_name, sizeof g_from_name, "CEL_MAIL_FROM_NAME");
    const char *tls = getenv("CEL_SMTP_TLS");
    if (tls) g_tls = !strcmp(tls, "none") ? TLS_NONE : !strcmp(tls, "try") ? TLS_TRY : TLS_REQUIRE;
    if (cel_mail_enabled()) {
        char safe_url[256]; redact_url(g_url, safe_url, sizeof safe_url);
        LOG_INFO("mailer: SMTP via %s (from %s)", safe_url, g_from);
    }
}

bool cel_mail_enabled(void) { return g_url[0] && g_from[0]; }

/* libcurl pulls the message body through this callback. */
typedef struct { const char *data; size_t len, sent; } upload_t;
static size_t payload_read(char *buf, size_t sz, size_t n, void *ud) {
    upload_t *u = ud;
    size_t room = sz * n;
    size_t left = u->len - u->sent;
    size_t take = left < room ? left : room;
    if (take) { memcpy(buf, u->data + u->sent, take); u->sent += take; }
    return take;
}

static bool has_crlf(const char *s) { return strchr(s, '\r') || strchr(s, '\n'); }

/* RFC 5322 Date, locale-independent (fixed English day/month names, UTC). */
static void rfc822_date(char *out, size_t n) {
    static const char *dow[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, n, "%s, %02d %s %04d %02d:%02d:%02d +0000",
             dow[tm.tm_wday], tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

int cel_mail_send(const char *to, const char *subject, const char *body) {
    return cel_mail_send_html(to, subject, body, NULL, NULL, NULL);
}

int cel_mail_send_from(const char *to, const char *subject, const char *body,
                       const char *from_addr, const char *from_name) {
    return cel_mail_send_html(to, subject, body, NULL, from_addr, from_name);
}

int cel_mail_send_html(const char *to, const char *subject,
                       const char *text_body, const char *html_body,
                       const char *from_addr, const char *from_name) {
    if (!cel_mail_enabled() || !to || !subject || !text_body) return -1;
    /* Per-app From (bundle _mail) overrides the process-wide From/name; SMTP creds
     * (g_url/g_user/g_pass) stay process-wide — secrets never come from a bundle. */
    const char *from  = (from_addr && *from_addr) ? from_addr : g_from;
    const char *fname = (from_name && *from_name) ? from_name : g_from_name;
    if (has_crlf(to) || has_crlf(subject) || has_crlf(from) || has_crlf(fname)) {  /* header-injection guard */
        LOG_WARN("mailer: refusing recipient/subject/from with CR/LF");
        return -1;
    }
    bool html = html_body && *html_body;   /* HTML present → multipart/alternative */

    char date[64];
    rfc822_date(date, sizeof date);
    char mid[33] = "cellar";
    cel_random_token_hex(mid, sizeof mid, 16);          /* best-effort Message-ID */
    char bnd[33] = "cel";
    cel_random_token_hex(bnd, sizeof bnd, 16);          /* random multipart boundary */
    const char *at = strchr(from, '@');

    size_t cap = strlen(text_body) + (html ? strlen(html_body) : 0)
               + strlen(subject) + strlen(to) + strlen(from) + strlen(fname) + 768;
    char *msg = malloc(cap);
    if (!msg) return -1;
    int len;
    if (html) {
        /* Body parts carry newlines legitimately (after the header separator), so
         * they are NOT CR/LF-guarded; only the headers above are. Random boundary. */
        len = snprintf(msg, cap,
            "Date: %s\r\n"
            "From: %s%s<%s>\r\n"
            "To: <%s>\r\n"
            "Subject: %s\r\n"
            "Message-ID: <%s@%s>\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/alternative; boundary=\"%s\"\r\n"
            "\r\n"
            "--%s\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "%s\r\n"
            "--%s\r\n"
            "Content-Type: text/html; charset=UTF-8\r\n"
            "\r\n"
            "%s\r\n"
            "--%s--\r\n",
            date, fname[0] ? fname : "", fname[0] ? " " : "", from,
            to, subject, mid, at ? at + 1 : "localhost", bnd,
            bnd, text_body, bnd, html_body, bnd);
    } else {
        len = snprintf(msg, cap,
            "Date: %s\r\n"
            "From: %s%s<%s>\r\n"
            "To: <%s>\r\n"
            "Subject: %s\r\n"
            "Message-ID: <%s@%s>\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "%s\r\n",
            date, fname[0] ? fname : "", fname[0] ? " " : "", from,
            to, subject, mid, at ? at + 1 : "localhost", text_body);
    }
    if (len < 0 || (size_t)len >= cap) { free(msg); return -1; }

    CURL *c = curl_easy_init();
    if (!c) { free(msg); return -1; }

    char envfrom[300], envrcpt[300];
    snprintf(envfrom, sizeof envfrom, "<%s>", from);
    snprintf(envrcpt, sizeof envrcpt, "<%s>", to);
    struct curl_slist *rcpt = curl_slist_append(NULL, envrcpt);
    upload_t up = { msg, (size_t)len, 0 };

    curl_easy_setopt(c, CURLOPT_URL, g_url);
    if (g_user[0]) {
        curl_easy_setopt(c, CURLOPT_USERNAME, g_user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, g_pass);
    }
    curl_easy_setopt(c, CURLOPT_USE_SSL,
                     g_tls == TLS_REQUIRE ? CURLUSESSL_ALL :
                     g_tls == TLS_TRY     ? CURLUSESSL_TRY : CURLUSESSL_NONE);
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, envfrom);
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, payload_read);
    curl_easy_setopt(c, CURLOPT_READDATA, &up);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) LOG_WARN("mailer: send to %s failed: %s", to, curl_easy_strerror(rc));

    curl_slist_free_all(rcpt);
    curl_easy_cleanup(c);
    free(msg);
    return rc == CURLE_OK ? 0 : -1;
}
