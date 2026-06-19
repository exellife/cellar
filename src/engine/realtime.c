#include "realtime.h"
#include "core/metrics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A single mutex-guarded linked list of subscriptions. Subscribe/unsubscribe run
 * on WS handler threads; publish runs on write (DB-pool) threads — hence the lock.
 * g_count is an atomic mirror so the write-path demand gate (pgf_realtime_active)
 * never has to take the lock. A per-table hash is the obvious scale follow-up. */
typedef struct rt_node {
    int fd;
    pgf_subscription_t sub;
    struct rt_node *next;
} rt_node_t;

static rt_node_t       *g_list   = NULL;
static pthread_mutex_t  g_lock   = PTHREAD_MUTEX_INITIALIZER;
static atomic_int       g_count  = 0;
static pgf_rt_send_fn   g_send   = NULL;
static pgf_rt_member_fn g_member = NULL;

void pgf_realtime_init(pgf_rt_send_fn send, pgf_rt_member_fn member) {
    g_send = send; g_member = member;
}

void pgf_realtime_cleanup(void) {
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

bool pgf_rt_row_matches(const pgf_rt_pred_t *preds, int npreds, const cJSON *row) {
    for (int i = 0; i < npreds; i++) {
        const pgf_rt_pred_t *p = &preds[i];
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

int pgf_realtime_subscribe(int fd, const pgf_subscription_t *sub) {
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

void pgf_realtime_unsubscribe(int fd, const char *table) {
    pthread_mutex_lock(&g_lock);
    int removed = remove_locked(fd, table);
    pthread_mutex_unlock(&g_lock);
    if (removed) atomic_fetch_sub(&g_count, removed);
}

void pgf_realtime_drop_conn(int fd) {
    pthread_mutex_lock(&g_lock);
    int removed = remove_locked(fd, NULL);
    pthread_mutex_unlock(&g_lock);
    if (removed) atomic_fetch_sub(&g_count, removed);
}

bool pgf_realtime_active(void) { return atomic_load(&g_count) > 0; }

long pgf_realtime_count(void) { return (long)atomic_load(&g_count); }

/* ---- delivery ------------------------------------------------------------- */

void pgf_realtime_publish(const char *table, const char *op, const cJSON *row) {
    if (atomic_load(&g_count) == 0 || !g_send || !row) return;
    pgf_metric_inc(PGF_M_RT_EVENTS);

    /* Collect matching subscribers under the lock, then send outside it (sends may
     * buffer / do I/O — don't hold the registry mutex across them). A VIA
     * (membership) subscription is NOT delivered on the flat predicate alone: its
     * membership is re-verified against the DB after the lock is released (M-5), so
     * a participant removed after subscribe stops receiving. EQ/OR subscriptions
     * deliver directly (no per-publish query). */
    pthread_mutex_lock(&g_lock);
    int cap = atomic_load(&g_count);             /* upper bound on matches */
    int *fds = cap > 0 ? malloc((size_t)cap * sizeof *fds) : NULL;
    pgf_subscription_t *via = cap > 0 ? malloc((size_t)cap * sizeof *via) : NULL;
    int *via_fd = cap > 0 ? malloc((size_t)cap * sizeof *via_fd) : NULL;
    int nf = 0, nv = 0;
    if (fds && via && via_fd) {
        for (rt_node_t *n = g_list; n; n = n->next) {
            if (strcmp(n->sub.table, table) != 0 ||
                !pgf_rt_row_matches(n->sub.preds, n->sub.npreds, row)) continue;
            if (n->sub.via) {            /* copy out for an out-of-lock re-check */
                if (nv < cap) { via[nv] = n->sub; via_fd[nv] = n->fd; nv++; }
            } else if (nf < cap) {
                fds[nf++] = n->fd;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);

    /* Re-authorize VIA subscribers against current membership, outside the lock.
     * Fail closed if no member callback is wired. */
    for (int i = 0; i < nv && nf < cap; i++)
        if (g_member && g_member(&via[i])) fds[nf++] = via_fd[i];
    free(via); free(via_fd);

    if (nf == 0) { free(fds); return; }

    /* Serialize the event once — every matching subscriber gets the same row. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "table", table);
    cJSON_AddStringToObject(o, "op", op);
    cJSON_AddItemReferenceToObject(o, "row", (cJSON *)row);   /* reference: row not owned */
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        size_t len = strlen(json);
        for (int i = 0; i < nf; i++) g_send(fds[i], json, len);
        free(json);
    }
    free(fds);
}
