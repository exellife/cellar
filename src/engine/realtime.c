#include "realtime.h"
#include "core/metrics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A single mutex-guarded linked list of subscriptions. Subscribe/unsubscribe run
 * on WS handler threads; publish runs on write (DB-pool) threads — hence the lock.
 * g_count is an atomic mirror so the write-path demand gate (cel_realtime_active)
 * never has to take the lock. A per-table hash is the obvious scale follow-up. */
typedef struct rt_node {
    int fd;
    cel_subscription_t sub;
    struct rt_node *next;
} rt_node_t;

static rt_node_t       *g_list   = NULL;
static pthread_mutex_t  g_lock   = PTHREAD_MUTEX_INITIALIZER;
static atomic_int       g_count  = 0;
static cel_rt_send_fn   g_send   = NULL;
static cel_rt_member_fn g_member = NULL;
static cel_rt_filter_fn g_filter = NULL;

void cel_realtime_init(cel_rt_send_fn send, cel_rt_member_fn member) {
    g_send = send; g_member = member;
}

void cel_realtime_set_filter(cel_rt_filter_fn filter) { g_filter = filter; }

void cel_realtime_cleanup(void) {
    pthread_mutex_lock(&g_lock);
    for (rt_node_t *n = g_list; n; ) { rt_node_t *nx = n->next; free(n); n = nx; }
    g_list = NULL;
    atomic_store(&g_count, 0);
    pthread_mutex_unlock(&g_lock);
}

/* ---- predicate matching (also used by the unit test) ---------------------- */

/* Compare a row field to a literal value as text (uuid/text strcmp; numbers and
 * bools rendered to text). Missing field never matches. */
static bool field_eq(const cJSON *row, const char *col, const char *value) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(row, col);
    if (!f) return false;
    if (cJSON_IsString(f)) return strcmp(f->valuestring, value) == 0;
    if (cJSON_IsBool(f))   return strcmp(cJSON_IsTrue(f) ? "true" : "false", value) == 0;
    if (cJSON_IsNumber(f)) {
        char b[40];
        double d = f->valuedouble;
        if (d == (double)(long long)d) snprintf(b, sizeof b, "%lld", (long long)d);
        else                           snprintf(b, sizeof b, "%.17g", d);
        return strcmp(b, value) == 0;
    }
    return false;
}

bool cel_rt_row_matches(const cel_rt_pred_t *preds, int npreds, const cJSON *row) {
    for (int i = 0; i < npreds; i++) {
        const cel_rt_pred_t *p = &preds[i];
        if (p->is_or) {
            bool any = false;
            for (int j = 0; j < p->ncols && !any; j++)
                if (field_eq(row, p->cols[j], p->value)) any = true;
            if (!any) return false;
        } else {
            if (!field_eq(row, p->column, p->value)) return false;
        }
    }
    return true;   /* npreds == 0 => match everything (superuser) */
}

/* ---- registry mutations --------------------------------------------------- */

/* Unlink every node matching fd and (table==NULL ? any : that table). Returns the
 * number removed. Caller holds the lock. */
static int remove_locked(int fd, const char *table) {
    int removed = 0;
    rt_node_t **pp = &g_list;
    while (*pp) {
        rt_node_t *n = *pp;
        if (n->fd == fd && (!table || strcmp(n->sub.table, table) == 0)) {
            *pp = n->next; free(n); removed++;
        } else {
            pp = &n->next;
        }
    }
    return removed;
}

int cel_realtime_subscribe(int fd, const cel_subscription_t *sub) {
    rt_node_t *n = malloc(sizeof *n);
    if (!n) return -1;
    n->fd = fd; n->sub = *sub;

    pthread_mutex_lock(&g_lock);
    int replaced = remove_locked(fd, sub->table);   /* re-subscribe replaces */
    n->next = g_list; g_list = n;
    pthread_mutex_unlock(&g_lock);

    if (!replaced) atomic_fetch_add(&g_count, 1);
    return 0;
}

void cel_realtime_unsubscribe(int fd, const char *table) {
    pthread_mutex_lock(&g_lock);
    int removed = remove_locked(fd, table);
    pthread_mutex_unlock(&g_lock);
    if (removed) atomic_fetch_sub(&g_count, removed);
}

void cel_realtime_drop_conn(int fd) {
    pthread_mutex_lock(&g_lock);
    int removed = remove_locked(fd, NULL);
    pthread_mutex_unlock(&g_lock);
    if (removed) atomic_fetch_sub(&g_count, removed);
}

bool cel_realtime_active(void) { return atomic_load(&g_count) > 0; }

long cel_realtime_count(void) { return (long)atomic_load(&g_count); }

/* ---- delivery ------------------------------------------------------------- */

void cel_realtime_publish(const void *app, const char *table, const char *op, const cJSON *row) {
    if (atomic_load(&g_count) == 0 || !g_send || !row) return;
    cel_metric_inc(CEL_M_RT_EVENTS);

    /* Collect predicate-matching subscribers (with a copy of each sub) under the
     * lock, then do membership re-check, the on_realtime filter, and the sends
     * OUTSIDE it — those run Lua / do I/O and must not hold the registry mutex. */
    pthread_mutex_lock(&g_lock);
    int cap = atomic_load(&g_count);             /* upper bound on matches */
    typedef struct { int fd; cel_subscription_t sub; } cand_t;
    cand_t *cand = cap > 0 ? malloc((size_t)cap * sizeof *cand) : NULL;
    int nc = 0;
    if (cand) {
        for (rt_node_t *n = g_list; n && nc < cap; n = n->next) {
            if (n->sub.app != app) continue;        /* isolation: only the writing app's subscribers */
            if (strcmp(n->sub.table, table) != 0 ||
                !cel_rt_row_matches(n->sub.preds, n->sub.npreds, row)) continue;
            cand[nc].fd = n->fd; cand[nc].sub = n->sub; nc++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (nc == 0) { free(cand); return; }

    /* Serialize the event once — every delivered subscriber gets the same row. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "table", table);
    cJSON_AddStringToObject(o, "op", op);
    cJSON_AddItemReferenceToObject(o, "row", (cJSON *)row);   /* reference: row not owned */
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) { free(cand); return; }
    size_t len = strlen(json);

    for (int i = 0; i < nc; i++) {
        const cel_subscription_t *s = &cand[i].sub;
        /* VIA: re-verify membership (M-5); fail closed if no member callback. */
        if (s->via && !(g_member && g_member(s))) continue;
        /* on_realtime: an optional per-subscriber delivery filter (the hook). */
        if (g_filter && !g_filter(s, table, op, row)) continue;
        g_send(cand[i].fd, json, len);
    }
    free(json);
    free(cand);
}
