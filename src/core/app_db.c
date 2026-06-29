/* ============================================================================
 * cellar — per-app SQLite handle layer (see app_db.h for the contract)
 * ============================================================================ */
#include "app_db.h"
#include "logger.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- one app: its file, a pool of WAL handles, and a writer lock ---------- */
struct app_db {
    char     path[4096];

    /* Connection pool. `conns[i]` is opened lazily; `in_use[i]` marks a borrow.
     * Guarded by pool_mtx; pool_cv wakes a waiter when a connection is freed. */
    sqlite3        *conns[CEL_APP_CONNS_PER_APP];
    bool            in_use[CEL_APP_CONNS_PER_APP];
    int             nconns;       /* how many of the slots have been opened    */
    int             checked_out;  /* how many are borrowed right now           */
    pthread_mutex_t pool_mtx;
    pthread_cond_t  pool_cv;

    /* Per-connection prepared-statement cache (sql text -> compiled stmt), so the
     * CRUD path parses + plans each distinct statement once, not once per request.
     * A slot's cache is touched only by the thread holding that connection (it's
     * checked out), so it needs no lock of its own. */
    struct { char *sql; sqlite3_stmt *st; } cache[CEL_APP_CONNS_PER_APP][CEL_STMT_CACHE_MAX];
    int             cache_n[CEL_APP_CONNS_PER_APP];

    /* Per-app writer serialization (design §6): one writer per file at a time. */
    pthread_mutex_t write_mtx;

    uint64_t last_used;           /* LRU tick, bumped on app_db_get            */
    int      refs;                /* outstanding references (g_reg.mtx-guarded);
                                   * a pinned handle is never evicted/freed     */
};

/* ---- global registry: a bounded, LRU-evicted set of open apps ------------- */
static struct {
    app_db_t       *apps[CEL_APP_MAX_OPEN];
    int             count;
    uint64_t        tick;
    pthread_mutex_t mtx;
    bool            inited;
} g_reg = { .mtx = PTHREAD_MUTEX_INITIALIZER };

/* The app a query runs against: a per-thread binding (set by HTTP request routing
 * for the duration of one request) overriding a process-wide default. WS workers
 * never bind, so they fall back to the default (the single-app deployment); HTTP
 * binds per request so concurrent requests hit different apps on the worker pool.
 * The thread-local needs no lock; the default is set once at boot. */
static app_db_t        *g_default = NULL;   /* process-wide fallback */
static __thread app_db_t *t_current = NULL; /* this thread's request binding */

int app_db_global_init(void)
{
    pthread_mutex_lock(&g_reg.mtx);
    g_reg.inited = true;
    pthread_mutex_unlock(&g_reg.mtx);
    return 0;
}

void app_db_set_current(app_db_t *db) { t_current = db; }
app_db_t *app_db_current(void) { return t_current ? t_current : g_default; }
void app_db_set_default(app_db_t *db) { g_default = db; }

/* Open and configure one connection for `path`. Caller holds no locks that the
 * PRAGMAs need. Returns an open handle or NULL (after logging). */
static sqlite3 *open_conn(const char *path)
{
    sqlite3 *c = NULL;
    /* NOMUTEX: each handle is used by one thread at a time (the pool guarantees
     * it), so SQLite skips its internal per-call mutex. CREATE so a brand-new
     * app's data.db is materialized on first write. */
    int rc = sqlite3_open_v2(path, &c,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("app_db: open %s failed: %s", path, c ? sqlite3_errmsg(c) : sqlite3_errstr(rc));
        sqlite3_close(c);
        return NULL;
    }

    /* Block (rather than erroring) for up to 5s if another connection holds the
     * write lock — paired with per-app write serialization this is just slack. */
    sqlite3_busy_timeout(c, 5000);

    /* Extended result codes so callers can tell a UNIQUE violation (→409) from a
     * FK / NOT NULL / CHECK violation (→400) on a failed statement. */
    sqlite3_extended_result_codes(c, 1);

    /* WAL: concurrent readers + a single writer; NORMAL sync is WAL-durable
     * enough (a crash can lose the last commit, not corrupt the file); enforce
     * foreign keys (off by default in SQLite). */
    char *err = NULL;
    static const char *pragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "PRAGMA busy_timeout=5000;";
    if (sqlite3_exec(c, pragmas, NULL, NULL, &err) != SQLITE_OK) {
        LOG_ERROR("app_db: PRAGMA setup on %s failed: %s", path, err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(c);
        return NULL;
    }
    return c;
}

static app_db_t *app_new(const char *path)
{
    app_db_t *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    snprintf(db->path, sizeof db->path, "%s", path);
    pthread_mutex_init(&db->pool_mtx, NULL);
    pthread_cond_init(&db->pool_cv, NULL);
    pthread_mutex_init(&db->write_mtx, NULL);
    return db;
}

static void app_free(app_db_t *db)
{
    for (int i = 0; i < db->nconns; i++) {
        /* finalize the connection's cached statements before closing it (sqlite3_close
         * refuses a connection with live statements). */
        for (int j = 0; j < db->cache_n[i]; j++) {
            sqlite3_finalize(db->cache[i][j].st);
            free(db->cache[i][j].sql);
        }
        if (db->conns[i]) sqlite3_close(db->conns[i]);
    }
    pthread_mutex_destroy(&db->pool_mtx);
    pthread_cond_destroy(&db->pool_cv);
    pthread_mutex_destroy(&db->write_mtx);
    free(db);
}

/* Caller holds g_reg.mtx. Drop the least-recently-used app that is both unpinned
 * (refs == 0) and has no borrowed connections. Returns true if one was evicted. */
static bool evict_idle_locked(void)
{
    int best = -1;
    uint64_t best_tick = UINT64_MAX;
    for (int i = 0; i < g_reg.count; i++) {
        app_db_t *a = g_reg.apps[i];
        if (a->refs != 0) continue;                 /* pinned — never evict (refs is g_reg.mtx-guarded) */
        pthread_mutex_lock(&a->pool_mtx);
        bool idle = (a->checked_out == 0);
        pthread_mutex_unlock(&a->pool_mtx);
        if (idle && a->last_used < best_tick) { best_tick = a->last_used; best = i; }
    }
    if (best < 0) return false;

    app_db_t *victim = g_reg.apps[best];
    g_reg.apps[best] = g_reg.apps[--g_reg.count];   /* swap-remove */
    LOG_DEBUG("app_db: evicting idle app %s", victim->path);
    app_free(victim);
    return true;
}

/* Caller holds g_reg.mtx. Find-or-create the app for `db_path`; take a reference
 * when `pin`. Returns NULL if the registry is full of unevictable apps or OOM. */
static app_db_t *get_locked(const char *db_path, bool pin)
{
    for (int i = 0; i < g_reg.count; i++) {
        if (strcmp(g_reg.apps[i]->path, db_path) == 0) {
            g_reg.apps[i]->last_used = ++g_reg.tick;
            if (pin) g_reg.apps[i]->refs++;
            return g_reg.apps[i];
        }
    }

    if (g_reg.count >= CEL_APP_MAX_OPEN && !evict_idle_locked()) {
        LOG_WARN("app_db: registry full (%d apps, none evictable); cannot open %s",
                 g_reg.count, db_path);
        return NULL;
    }

    app_db_t *db = app_new(db_path);
    if (!db) return NULL;
    db->last_used = ++g_reg.tick;
    if (pin) db->refs++;
    g_reg.apps[g_reg.count++] = db;
    return db;
}

app_db_t *app_db_get(const char *db_path)
{
    pthread_mutex_lock(&g_reg.mtx);
    app_db_t *d = get_locked(db_path, false);
    pthread_mutex_unlock(&g_reg.mtx);
    return d;
}

app_db_t *app_db_get_pinned(const char *db_path)
{
    pthread_mutex_lock(&g_reg.mtx);
    app_db_t *d = get_locked(db_path, true);
    pthread_mutex_unlock(&g_reg.mtx);
    return d;
}

void app_db_unref(app_db_t *db)
{
    if (!db) return;
    pthread_mutex_lock(&g_reg.mtx);
    if (db->refs > 0) db->refs--;
    pthread_mutex_unlock(&g_reg.mtx);
}

sqlite3 *app_db_conn_acquire(app_db_t *db)
{
    pthread_mutex_lock(&db->pool_mtx);
    for (;;) {
        /* 1. an already-open, free handle */
        for (int i = 0; i < db->nconns; i++) {
            if (!db->in_use[i]) {
                db->in_use[i] = true;
                db->checked_out++;
                sqlite3 *c = db->conns[i];
                pthread_mutex_unlock(&db->pool_mtx);
                return c;
            }
        }
        /* 2. room to open a new one */
        if (db->nconns < CEL_APP_CONNS_PER_APP) {
            int slot = db->nconns;
            sqlite3 *c = open_conn(db->path);   /* under pool_mtx: simple + race-free */
            if (!c) { pthread_mutex_unlock(&db->pool_mtx); return NULL; }
            db->conns[slot] = c;
            db->in_use[slot] = true;
            db->nconns++;
            db->checked_out++;
            pthread_mutex_unlock(&db->pool_mtx);
            return c;
        }
        /* 3. pool exhausted — wait for a release */
        pthread_cond_wait(&db->pool_cv, &db->pool_mtx);
    }
}

void app_db_conn_release(app_db_t *db, sqlite3 *conn)
{
    pthread_mutex_lock(&db->pool_mtx);
    for (int i = 0; i < db->nconns; i++) {
        if (db->conns[i] == conn) {
            db->in_use[i] = false;
            db->checked_out--;
            pthread_cond_signal(&db->pool_cv);
            break;
        }
    }
    pthread_mutex_unlock(&db->pool_mtx);
}

sqlite3_stmt *app_db_stmt_cached(app_db_t *db, sqlite3 *conn, const char *sql)
{
    /* Find the slot holding `conn`. It's checked out to this thread, so conns[slot]
     * == conn is stable and that slot's cache is ours; other slots may be opened
     * concurrently, but an aligned pointer read is atomic and `conn` is unique, so
     * the lock-free scan resolves the right slot. nconns only grows, and our conn
     * was opened before we borrowed it, so it's within the count we read. */
    int slot = -1, n = db->nconns;
    for (int i = 0; i < n; i++) if (db->conns[i] == conn) { slot = i; break; }
    if (slot < 0) return NULL;

    for (int i = 0; i < db->cache_n[slot]; i++) {
        if (strcmp(db->cache[slot][i].sql, sql) == 0) {      /* hit: reuse */
            sqlite3_stmt *st = db->cache[slot][i].st;
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            return st;
        }
    }

    sqlite3_stmt *st = NULL;                                  /* miss: compile once, cache */
    if (sqlite3_prepare_v3(conn, sql, -1, SQLITE_PREPARE_PERSISTENT, &st, NULL) != SQLITE_OK)
        return NULL;
    char *key = strdup(sql);
    if (!key) { sqlite3_finalize(st); return NULL; }          /* every returned stmt must be cached */

    int *cn = &db->cache_n[slot];
    if (*cn >= CEL_STMT_CACHE_MAX) {                           /* FIFO-evict the oldest */
        sqlite3_finalize(db->cache[slot][0].st);
        free(db->cache[slot][0].sql);
        memmove(&db->cache[slot][0], &db->cache[slot][1],
                (size_t)(CEL_STMT_CACHE_MAX - 1) * sizeof db->cache[slot][0]);
        (*cn)--;
    }
    db->cache[slot][*cn].sql = key;
    db->cache[slot][*cn].st  = st;
    (*cn)++;
    return st;
}

/* Per-thread depth of held app write locks. The mutex is non-recursive, so a
 * worker that already holds it (CRUD before/after/resolve hooks, job hooks) must
 * not re-acquire it — app_db_in_write_lock lets such paths detect + refuse that. */
static __thread int t_wlock_depth = 0;
void app_db_write_lock(app_db_t *db)   { pthread_mutex_lock(&db->write_mtx); t_wlock_depth++; }
void app_db_write_unlock(app_db_t *db) { t_wlock_depth--; pthread_mutex_unlock(&db->write_mtx); }
int  app_db_in_write_lock(void)        { return t_wlock_depth > 0; }

int app_db_exec(app_db_t *db, const char *sql, char **errmsg)
{
    app_db_write_lock(db);
    sqlite3 *c = app_db_conn_acquire(db);
    if (!c) {
        app_db_write_unlock(db);
        if (errmsg) *errmsg = NULL;
        return SQLITE_CANTOPEN;
    }
    int rc = sqlite3_exec(c, sql, NULL, NULL, errmsg);
    app_db_conn_release(db, c);
    app_db_write_unlock(db);
    return rc;
}

const char *app_db_path(const app_db_t *db) { return db->path; }

int app_db_open_count(void)
{
    pthread_mutex_lock(&g_reg.mtx);
    int n = g_reg.count;
    pthread_mutex_unlock(&g_reg.mtx);
    return n;
}

void app_db_global_shutdown(void)
{
    pthread_mutex_lock(&g_reg.mtx);
    for (int i = 0; i < g_reg.count; i++) app_free(g_reg.apps[i]);
    g_reg.count = 0;
    g_reg.inited = false;
    pthread_mutex_unlock(&g_reg.mtx);
}
