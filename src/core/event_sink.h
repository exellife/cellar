/* ============================================================================
 * cellar — EventSink: append-only behavioral event collection, behind a port.
 *
 * A generic engine primitive: record domain-agnostic events (a `type` string +
 * optional actor/subject ids + arbitrary JSON props) and read them back in a
 * monotonic order for consumers (rollups, recommendations, analytics). The
 * engine depends only on this interface; the default adapter appends to a SQLite
 * table, but a stream/warehouse adapter implements the same vtable.
 *
 * Port-first discipline: nothing about SQLite (or any store) leaks through this
 * header. The sink never learns a domain noun — "listing_viewed" is just a
 * `type` string the caller chose.
 *
 * Why collect from day one: behavioral history (views/searches/clicks) cannot be
 * backfilled. Emit is the cheap, high-volume path; read drains events to a
 * consumer (typically a JobQueue rollup) by ascending id cursor.
 * ============================================================================ */
#ifndef CEL_EVENT_SINK_H
#define CEL_EVENT_SINK_H

#include <stddef.h>

enum {
    EVT_OK     =  0,
    EVT_EINVAL = -1,   /* bad argument (e.g. NULL/empty type)            */
    EVT_EIO    = -2,   /* underlying store error                          */
    EVT_ENOMEM = -3    /* allocation failed                               */
};

/* An event to emit. `type` is required; the rest are optional (NULL = unset).
 * `props` is opaque JSON text the sink stores verbatim (caller's schema). */
typedef struct {
    const char *type;
    const char *actor_id;     /* who triggered it (user id / session); NULL ok */
    const char *subject_id;   /* the primary entity it's about; NULL ok        */
    const char *props;        /* arbitrary JSON properties; NULL ok            */
} event_t;

/* Read callback, invoked once per stored event in ascending id order. `id` is
 * the monotonic cursor; `ts` is unix seconds (sink-stamped at emit). The `ev`
 * fields point into transient storage — copy anything you retain. Return 0 to
 * continue, non-zero to stop iteration early. */
typedef int (*event_visit_fn)(void *user, long long id, long long ts, const event_t *ev);

/* The port: a vtable + opaque ctx. */
typedef struct event_sink {
    void *ctx;

    /* Append one event (sink stamps the timestamp + assigns the id). Returns
     * EVT_OK or a negative code. Thread-safe. */
    int (*emit)(void *ctx, const event_t *ev);

    /* Read up to `limit` events with id > `after_id`, ascending, invoking
     * `visit` for each. Returns the number visited (>= 0) or a negative code.
     * The caller advances its cursor to the last id seen. */
    long long (*read)(void *ctx, long long after_id, int limit,
                      event_visit_fn visit, void *user);

    /* Release adapter resources (does NOT close a caller-owned store handle). */
    void (*destroy)(void *ctx);
} event_sink_t;

/* ---- default adapter: SQLite append table --------------------------------
 * Appends to an `event` table in `db` (created if absent). `db` is a sqlite3*
 * (typed void* so this header carries no SQLite dependency); it is owned by the
 * caller and must outlive the sink. Thread-safe via an internal mutex (so it is
 * safe on a NOMUTEX connection). Returns EVT_OK and fills `*out`, or a negative
 * code. Free with out->destroy(out->ctx). */
int event_sink_sqlite_open(void *db, event_sink_t *out);

#endif /* CEL_EVENT_SINK_H */
