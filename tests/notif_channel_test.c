/* NotifChannel registry unit test — dependency-free (no DB/crypto). Pins the port:
 * register / get / same-name replace / unknown→NULL / null-guards, and that send()
 * routes to the adapter (recipient + msg + error propagation). */
#include "notif_channel.h"

#include <stdio.h>
#include <string.h>

static int   g_calls;
static char  g_last_to[128];
static char  g_last_title[128];

static int cap_send(const char *recipient, const cel_notif_msg_t *msg, char *err, int errlen) {
    (void)err; (void)errlen;
    g_calls++;
    snprintf(g_last_to, sizeof g_last_to, "%s", recipient ? recipient : "");
    snprintf(g_last_title, sizeof g_last_title, "%s", (msg && msg->title) ? msg->title : "");
    return 0;
}
static int fail_send(const char *recipient, const cel_notif_msg_t *msg, char *err, int errlen) {
    (void)recipient; (void)msg;
    if (err && errlen) snprintf(err, errlen, "boom");
    return -1;
}

static int fails = 0;
#define CHK(cond, msg) do { if (cond) printf("  ok   %s\n", msg); \
                            else { printf("  FAIL %s\n", msg); fails++; } } while (0)

int main(void) {
    printf("NotifChannel registry\n");

    CHK(cel_notif_get("email") == NULL, "unknown channel -> NULL");
    CHK(cel_notif_get(NULL) == NULL,    "get(NULL) -> NULL");

    static const cel_notif_channel_t email = { "email", cap_send };
    cel_notif_register(&email);
    CHK(cel_notif_get("email") == &email, "registered channel is found");
    CHK(cel_notif_get("webpush") == NULL, "other name still NULL");

    /* same name replaces (idempotent boot / test override) */
    static const cel_notif_channel_t email2 = { "email", fail_send };
    cel_notif_register(&email2);
    CHK(cel_notif_get("email") == &email2, "same-name register replaces");

    /* send() routes to the adapter; error propagates */
    cel_notif_msg_t msg = { .title = "Hi", .body = "there" };
    char err[64] = {0};
    int rc = cel_notif_get("email")->send("a@b.com", &msg, err, sizeof err);
    CHK(rc == -1 && strcmp(err, "boom") == 0, "send routes to adapter + err propagates");

    /* a second distinct channel coexists */
    static const cel_notif_channel_t wp = { "webpush", cap_send };
    cel_notif_register(&wp);
    CHK(cel_notif_get("webpush") == &wp, "second channel registered");
    g_calls = 0;
    cel_notif_get("webpush")->send("sub-json", &msg, err, sizeof err);
    CHK(g_calls == 1 && strcmp(g_last_to, "sub-json") == 0 && strcmp(g_last_title, "Hi") == 0,
        "capture adapter got recipient + title");

    /* null-guards: a NULL channel, or one with no send(), is ignored */
    cel_notif_register(NULL);
    static const cel_notif_channel_t bad = { "bad", NULL };
    cel_notif_register(&bad);
    CHK(cel_notif_get("bad") == NULL, "register without send() is ignored");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
