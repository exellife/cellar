/* ============================================================================
 * pgforge — realtime change events (subscription registry + authz fan-out).
 *
 * A WebSocket client SUBSCRIBEs to a realtime-enabled table; thereafter every
 * create/update/delete that it is ALLOWED TO SEE is pushed to it as a CHANGE.
 * "Allowed to see" reuses the same access rules as LIST (see api.c): the engine
 * resolves the caller's scope, binds it to concrete values, and hands this module
 * a set of delivery PREDICATES. Delivery is then a pure in-memory match of each
 * change row against every subscriber's predicates — no per-event DB query.
 *
 * Source-agnostic: this module only exposes pgf_realtime_publish(), called today
 * by an in-process emit from the write path. A LISTEN/NOTIFY (or WAL) source can
 * feed the SAME entry point later without touching the registry or the protocol.
 * ============================================================================ */
#ifndef PGF_REALTIME_H
#define PGF_REALTIME_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

#define PGF_RT_MAX_OR     4
#define PGF_RT_MAX_PREDS  4    /* tenant + owner (+headroom) — mirrors PGF_MAX_SCOPE */
#define PGF_RT_IDENT     64
#define PGF_RT_VALUE    128

/* A delivery predicate. The subscriber receives a change only if the row matches
 * EVERY predicate (an empty set matches everything — a superuser sees all rows).
 *   EQ : row[column] == value
 *   OR : row[cols[i]] == value for some i
 * `value` is concrete — the subscriber's user/tenant id, or a key value they were
 * authorized for at subscribe time (a VIA membership collapses to EQ once proven). */
typedef struct {
    bool is_or;
    char column[PGF_RT_IDENT];               /* EQ */
    char cols[PGF_RT_MAX_OR][PGF_RT_IDENT];  /* OR */
    int  ncols;
    char value[PGF_RT_VALUE];
} pgf_rt_pred_t;

/* A registered subscription: a table plus the predicates that gate delivery. */
typedef struct {
    char table[PGF_RT_IDENT];
    pgf_rt_pred_t preds[PGF_RT_MAX_PREDS];
    int  npreds;
    /* VIA (membership) gating (M-5): the flat predicate above pins the row to the
     * subscribed key (e.g. conversation_id), but membership can be revoked AFTER
     * subscribe — so a VIA subscription is re-authorized against the membership
     * table on every publish. These fields carry what that re-check needs; `via`
     * is false for EQ/OR subscriptions (no per-publish DB query). */
    bool via;
    char via_table[PGF_RT_IDENT];
    char via_ref[PGF_RT_IDENT];
    char via_user[PGF_RT_IDENT];
    char via_key[PGF_RT_VALUE];   /* the ref value subscribed to (e.g. conversation id) */
    char user_id[PGF_RT_VALUE];   /* the membership subject (the caller) */
    char tenant[PGF_RT_VALUE];    /* tenant context for the re-check (L-4); "" = none */
} pgf_subscription_t;

/* The transport push: deliver a framed CHANGE carrying `json` to connection fd. */
typedef void (*pgf_rt_send_fn)(int fd, const char *json, size_t len);
/* Re-authorize a VIA subscription at publish time: true iff the subscriber is
 * STILL a member (a fresh membership query). NULL => VIA subs never deliver. */
typedef bool (*pgf_rt_member_fn)(const pgf_subscription_t *sub);

void pgf_realtime_init(pgf_rt_send_fn send, pgf_rt_member_fn member);
void pgf_realtime_cleanup(void);

/* Register (or replace) the subscription for (fd, sub->table). Returns 0 on ok. */
int  pgf_realtime_subscribe(int fd, const pgf_subscription_t *sub);
/* Remove the (fd, table) subscription, if any. */
void pgf_realtime_unsubscribe(int fd, const char *table);
/* Remove every subscription for fd (call on disconnect). */
void pgf_realtime_drop_conn(int fd);

/* True if any subscription exists anywhere — the demand gate that keeps the write
 * path free of realtime cost when nobody is listening. */
bool pgf_realtime_active(void);

/* Current number of active subscriptions (for metrics). */
long pgf_realtime_count(void);

/* Deliver a change for `table` (op = "INSERT" | "UPDATE" | "DELETE") to every
 * matching subscriber. `row` is borrowed (not retained). */
void pgf_realtime_publish(const char *table, const char *op, const cJSON *row);

/* Exposed for unit testing: does `row` satisfy all of `preds`? */
bool pgf_rt_row_matches(const pgf_rt_pred_t *preds, int npreds, const cJSON *row);

#endif /* PGF_REALTIME_H */
