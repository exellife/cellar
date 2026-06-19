/* cellar — realtime registry + authz fan-out test (no DB).
 *
 * Exercises the part of #53 that decides WHO receives a change: predicate
 * matching (EQ / OR / empty=all) and the subscription registry (subscribe /
 * replace / unsubscribe / drop_conn / demand gate). A mock send callback records
 * which fds a publish reaches, so the fan-out is asserted deterministically with
 * neither a DB nor a socket. Links realtime.c alone.
 */
#include "realtime.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static void chk(const char *label, int cond) {
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", label);
    if (!cond) failures++;
}

/* Two app identities (the per-app handle a subscription belongs to). Publish only
 * reaches subscribers of the publishing app. */
#define APP1 ((const void *)1)
#define APP2 ((const void *)2)

/* ---- mock transport: record (fd, json) per publish ---- */
#define MAXCAP 64
static struct { int fd; char json[512]; } g_cap[MAXCAP];
static int g_ncap;
static void mock_send(int fd, const char *json, size_t len) {
    if (g_ncap < MAXCAP) {
        g_cap[g_ncap].fd = fd;
        snprintf(g_cap[g_ncap].json, sizeof g_cap[g_ncap].json, "%.*s", (int)len, json);
        g_ncap++;
    }
}
static void reset_cap(void) { g_ncap = 0; }
static int got(int fd) { for (int i = 0; i < g_ncap; i++) if (g_cap[i].fd == fd) return 1; return 0; }

/* ---- mock membership re-check (M-5): controllable "still a member?" ---- */
static int g_member_ok = 1;     /* what mock_member returns */
static int g_member_calls = 0;  /* how many times it was consulted */
static bool mock_member(const cel_subscription_t *sub) { (void)sub; g_member_calls++; return g_member_ok != 0; }

static cel_subscription_t S; /* scratch builder */
static const cel_subscription_t *sub_none(const char *table) {
    memset(&S, 0, sizeof S); S.app = APP1; snprintf(S.table, sizeof S.table, "%s", table); S.npreds = 0;
    return &S;
}
static const cel_subscription_t *sub_eq(const char *table, const char *col, const char *val) {
    memset(&S, 0, sizeof S); S.app = APP1; snprintf(S.table, sizeof S.table, "%s", table);
    S.preds[0].is_or = false;
    snprintf(S.preds[0].column, sizeof S.preds[0].column, "%s", col);
    snprintf(S.preds[0].value,  sizeof S.preds[0].value,  "%s", val);
    S.npreds = 1; return &S;
}
static const cel_subscription_t *sub_eq2(const char *table, const char *c1, const char *v1,
                                                              const char *c2, const char *v2) {
    sub_eq(table, c1, v1);
    S.preds[1].is_or = false;
    snprintf(S.preds[1].column, sizeof S.preds[1].column, "%s", c2);
    snprintf(S.preds[1].value,  sizeof S.preds[1].value,  "%s", v2);
    S.npreds = 2; return &S;
}
static const cel_subscription_t *sub_or(const char *table, const char *a, const char *b, const char *val) {
    memset(&S, 0, sizeof S); S.app = APP1; snprintf(S.table, sizeof S.table, "%s", table);
    S.preds[0].is_or = true; S.preds[0].ncols = 2;
    snprintf(S.preds[0].cols[0], sizeof S.preds[0].cols[0], "%s", a);
    snprintf(S.preds[0].cols[1], sizeof S.preds[0].cols[1], "%s", b);
    snprintf(S.preds[0].value,   sizeof S.preds[0].value,   "%s", val);
    S.npreds = 1; return &S;
}

/* A VIA (membership) subscription: a flat predicate (col == val) PLUS the via
 * coordinates that make the publish path re-verify membership (M-5). */
static const cel_subscription_t *sub_via(const char *table, const char *col, const char *val) {
    sub_eq(table, col, val);
    S.via = true;
    snprintf(S.via_table, sizeof S.via_table, "conversations");
    snprintf(S.via_ref,   sizeof S.via_ref,   "conv_id");
    snprintf(S.via_user,  sizeof S.via_user,  "user_id");
    snprintf(S.via_key,   sizeof S.via_key,   "%s", val);
    snprintf(S.user_id,   sizeof S.user_id,   "uX");
    return &S;
}

static cJSON *row1(const char *col, const char *val) {
    cJSON *r = cJSON_CreateObject(); cJSON_AddStringToObject(r, col, val); return r;
}

int main(void) {
    printf("realtime registry + fan-out\n");

    /* ---- predicate matching (pure) ---- */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "owner_id", "u1");
    cJSON_AddNumberToObject(r, "channel_id", 42);
    cel_rt_pred_t eq = { .is_or = false }; snprintf(eq.column, 64, "owner_id"); snprintf(eq.value, 128, "u1");
    cel_rt_pred_t no = { .is_or = false }; snprintf(no.column, 64, "owner_id"); snprintf(no.value, 128, "u2");
    cel_rt_pred_t num= { .is_or = false }; snprintf(num.column,64, "channel_id"); snprintf(num.value,128, "42");
    cel_rt_pred_t miss={ .is_or = false }; snprintf(miss.column,64,"nope"); snprintf(miss.value,128,"x");
    chk("EQ match",        cel_rt_row_matches(&eq, 1, r));
    chk("EQ no-match",    !cel_rt_row_matches(&no, 1, r));
    chk("EQ numeric",      cel_rt_row_matches(&num, 1, r));
    chk("missing field",  !cel_rt_row_matches(&miss, 1, r));
    chk("empty=all",       cel_rt_row_matches(&eq, 0, r));
    cJSON_Delete(r);

    /* ---- registry + fan-out ---- */
    cel_realtime_init(mock_send, NULL);
    chk("inactive at start", !cel_realtime_active());

    cel_realtime_subscribe(1, sub_eq("messages", "owner_id", "u1"));
    cel_realtime_subscribe(2, sub_eq("messages", "owner_id", "u2"));
    cel_realtime_subscribe(3, sub_or("messages", "rider_id", "driver_id", "u3"));
    cel_realtime_subscribe(4, sub_none("messages"));                 /* superuser: all */
    cel_realtime_subscribe(5, sub_eq2("messages", "tenant_id", "t1", "owner_id", "u5"));
    chk("active after subs", cel_realtime_active());

    /* owner u1's row -> fd1 (owner match) + fd4 (all); not 2/3/5 */
    reset_cap();
    cJSON *m1 = row1("owner_id", "u1");
    cel_realtime_publish(APP1, "messages", "INSERT", m1);
    chk("u1 -> fd1", got(1));   chk("u1 -> fd4", got(4));
    chk("u1 !-> fd2", !got(2)); chk("u1 !-> fd3", !got(3)); chk("u1 !-> fd5", !got(5));
    chk("payload has op", strstr(g_cap[0].json, "INSERT") != NULL);
    cJSON_Delete(m1);

    /* OR: a row where driver_id=u3 -> fd3 + fd4 */
    reset_cap();
    cJSON *m2 = row1("driver_id", "u3");
    cel_realtime_publish(APP1, "messages", "UPDATE", m2);
    chk("u3 -> fd3", got(3)); chk("u3 -> fd4", got(4)); chk("u3 !-> fd1", !got(1));
    cJSON_Delete(m2);

    /* tenant+owner: must satisfy BOTH preds */
    reset_cap();
    cJSON *m3 = cJSON_CreateObject();
    cJSON_AddStringToObject(m3, "tenant_id", "t1"); cJSON_AddStringToObject(m3, "owner_id", "u5");
    cel_realtime_publish(APP1, "messages", "INSERT", m3);
    chk("t1/u5 -> fd5", got(5)); chk("t1/u5 -> fd4", got(4));
    cJSON_Delete(m3);
    reset_cap();
    cJSON *m4 = cJSON_CreateObject();
    cJSON_AddStringToObject(m4, "tenant_id", "t2"); cJSON_AddStringToObject(m4, "owner_id", "u5");
    cel_realtime_publish(APP1, "messages", "INSERT", m4);
    chk("wrong tenant !-> fd5", !got(5));   /* owner matches but tenant doesn't */
    cJSON_Delete(m4);

    /* different table -> nobody (subs are per-table) */
    reset_cap();
    cJSON *m5 = row1("owner_id", "u1");
    cel_realtime_publish(APP1, "orders", "INSERT", m5);
    chk("other table -> none", g_ncap == 0);
    cJSON_Delete(m5);

    /* re-subscribe replaces: fd1 now scoped to u9, no longer u1 */
    cel_realtime_subscribe(1, sub_eq("messages", "owner_id", "u9"));
    reset_cap();
    cJSON *m6 = row1("owner_id", "u1");
    cel_realtime_publish(APP1, "messages", "INSERT", m6);
    chk("after replace u1 !-> fd1", !got(1));
    cJSON_Delete(m6);

    /* drop_conn(4) removes the catch-all */
    cel_realtime_drop_conn(4);
    reset_cap();
    cJSON *m7 = row1("owner_id", "u2");
    cel_realtime_publish(APP1, "messages", "INSERT", m7);
    chk("u2 -> fd2", got(2)); chk("after drop, !-> fd4", !got(4));
    cJSON_Delete(m7);

    /* unsubscribe(2) */
    cel_realtime_unsubscribe(2, "messages");
    reset_cap();
    cJSON *m8 = row1("owner_id", "u2");
    cel_realtime_publish(APP1, "messages", "INSERT", m8);
    chk("after unsubscribe, !-> fd2", !got(2));
    cJSON_Delete(m8);

    /* ---- C-2: app isolation — a subscriber of app B never sees app A's writes,
     * even on an identically-named table with a matching predicate ---- */
    sub_eq("messages", "owner_id", "u1"); S.app = APP2;   /* same table+pred, different app */
    cel_realtime_subscribe(20, &S);
    reset_cap();
    cJSON *x1 = row1("owner_id", "u1");
    cel_realtime_publish(APP1, "messages", "INSERT", x1);  /* app A writes */
    chk("cross-app: APP1 write !-> APP2 sub (fd20)", !got(20));
    reset_cap();
    cel_realtime_publish(APP2, "messages", "INSERT", x1);  /* app B writes */
    chk("same-app: APP2 write -> APP2 sub (fd20)", got(20));
    cJSON_Delete(x1);
    cel_realtime_drop_conn(20);

    /* ---- M-5: VIA subscriptions are re-authorized against membership on publish ---- */
    cel_realtime_init(mock_send, mock_member);          /* now with a membership re-check */
    cel_realtime_subscribe(10, sub_via("messages", "conv_id", "cX"));

    /* still a member -> the flat predicate matches AND the re-check passes -> delivered */
    g_member_ok = 1; g_member_calls = 0; reset_cap();
    cJSON *v1 = row1("conv_id", "cX");
    cel_realtime_publish(APP1, "messages", "INSERT", v1);
    chk("VIA member -> delivered", got(10));
    chk("VIA membership re-checked on publish", g_member_calls == 1);
    cJSON_Delete(v1);

    /* membership revoked -> flat predicate STILL matches, but the re-check fails -> NOT
     * delivered. This is the M-5 fix: a removed participant stops receiving. */
    g_member_ok = 0; reset_cap();
    cJSON *v2 = row1("conv_id", "cX");
    cel_realtime_publish(APP1, "messages", "INSERT", v2);
    chk("VIA non-member !-> delivered (M-5)", !got(10));
    cJSON_Delete(v2);

    /* a VIA sub fails closed when no membership callback is wired */
    cel_realtime_init(mock_send, NULL);
    g_member_ok = 1; reset_cap();
    cJSON *v3 = row1("conv_id", "cX");
    cel_realtime_publish(APP1, "messages", "INSERT", v3);
    chk("VIA fails closed without member cb", !got(10));
    cJSON_Delete(v3);

    cel_realtime_cleanup();
    chk("inactive after cleanup", !cel_realtime_active());

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
