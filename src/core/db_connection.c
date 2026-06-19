/**
 * @file db_connection.c
 * @brief PostgreSQL Database Connection Pool Implementation
 */

#include "db_connection.h"
#include "logger.h"
#include "metrics.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

/* ============================================================================
 * Connection Pool State
 * ============================================================================ */

/* Per-connection prepared-statement cache. A pooled connection is held
 * exclusively by one thread between acquire/release, so its slot needs no
 * locking; connections[] is immutable after init, so slot lookup is lock-free
 * too. Keyed by SQL text (hash + a verifying strcmp copy, so a hash collision
 * can never bind the wrong plan). Statement i is named "cel_s<i>". */
#define STMT_CACHE_MAX 64
typedef struct {
    uint64_t hash[STMT_CACHE_MAX];   /* fnv1a-64 of the SQL text */
    char    *sql[STMT_CACHE_MAX];    /* owned copy, for collision-safe compare */
    int      count;
} stmt_cache_t;

typedef struct {
    PGconn **connections;      // Array of connection pointers
    bool *available;           // Availability flags
    stmt_cache_t *stmts;       // Parallel to connections[]: per-conn plan cache
    bool prepared_enabled;     // CEL_PREPARED_STATEMENTS != 0
    bool pipeline_enabled;     // CEL_DB_PIPELINE == 1 (opt-in; needs libpq pipelining)
    int pool_size;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool initialized;
} db_pool_t;

static db_pool_t g_pool = {0};

/* ---- prepared-statement cache helpers ------------------------------------- */

static uint64_t sql_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;          /* FNV-1a 64 */
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* Index of `conn` in the pool, or -1. connections[] is fixed after init. */
static int slot_of(PGconn *conn) {
    for (int i = 0; i < g_pool.pool_size; i++)
        if (g_pool.connections[i] == conn) return i;
    return -1;
}

/* Forget every cached statement for a slot. Used when a connection is reset
 * (PQreset wipes the server's prepared statements, so our bookkeeping must
 * follow or we'd PQexecPrepared a name the server no longer knows). Does NOT
 * issue DEALLOCATE — the caller has already lost/replaced the server state. */
static void stmt_cache_clear_slot(int slot) {
    if (slot < 0 || !g_pool.stmts) return;
    stmt_cache_t *sc = &g_pool.stmts[slot];
    for (int i = 0; i < sc->count; i++) { free(sc->sql[i]); sc->sql[i] = NULL; }
    sc->count = 0;
}

/* ============================================================================
 * Pool Management
 * ============================================================================ */

int db_connection_pool_init(const char *conninfo, int pool_size) {
    if (g_pool.initialized) {
        LOG_WARN("Database connection pool already initialized");
        return 0;
    }

    if (!conninfo || pool_size <= 0) {
        LOG_ERROR("Invalid parameters for connection pool");
        return -1;
    }

    LOG_INFO("Initializing database connection pool (size: %d)", pool_size);

    // Allocate arrays
    g_pool.connections = calloc(pool_size, sizeof(PGconn *));
    g_pool.available = calloc(pool_size, sizeof(bool));
    g_pool.stmts = calloc(pool_size, sizeof(stmt_cache_t));

    if (!g_pool.connections || !g_pool.available || !g_pool.stmts) {
        LOG_ERROR("Failed to allocate connection pool");
        free(g_pool.connections);
        free(g_pool.available);
        free(g_pool.stmts);
        return -1;
    }

    /* Prepared-statement caching is on by default; CEL_PREPARED_STATEMENTS=0 opts out. */
    const char *pe = getenv("CEL_PREPARED_STATEMENTS");
    g_pool.prepared_enabled = !(pe && strcmp(pe, "0") == 0);
    LOG_INFO("Prepared-statement cache: %s", g_pool.prepared_enabled ? "enabled" : "disabled");

    /* Pooled-mode pipelining is opt-in (CEL_DB_PIPELINE=1): it collapses the
     * BEGIN/set_config/query/COMMIT round trips into one. Needs libpq pipelining
     * (>= 14) and the prepared-statement cache (it sends PQsendQueryPrepared). */
#ifdef LIBPQ_HAS_PIPELINING
    const char *pl = getenv("CEL_DB_PIPELINE");
    g_pool.pipeline_enabled = (pl && strcmp(pl, "1") == 0) && g_pool.prepared_enabled;
#else
    g_pool.pipeline_enabled = false;
#endif
    if (g_pool.pipeline_enabled) LOG_INFO("Pooled-mode pipelining: enabled");

    g_pool.pool_size = pool_size;
    pthread_mutex_init(&g_pool.lock, NULL);
    pthread_cond_init(&g_pool.cond, NULL);

    // Create connections
    int connected = 0;
    for (int i = 0; i < pool_size; i++) {
        g_pool.connections[i] = PQconnectdb(conninfo);
        
        if (PQstatus(g_pool.connections[i]) != CONNECTION_OK) {
            LOG_ERROR("Connection %d failed: %s", i, PQerrorMessage(g_pool.connections[i]));
            PQfinish(g_pool.connections[i]);
            g_pool.connections[i] = NULL;
            g_pool.available[i] = false;
        } else {
            g_pool.available[i] = true;
            connected++;
            LOG_DEBUG("Connection %d established", i);
        }
    }

    if (connected == 0) {
        LOG_ERROR("Failed to establish any database connections");
        db_connection_pool_cleanup();
        return -1;
    }

    g_pool.initialized = true;
    LOG_INFO("Database connection pool initialized: %d/%d connections active", 
             connected, pool_size);
    
    return 0;
}

PGconn *db_connection_acquire(void) {
    if (!g_pool.initialized) {
        LOG_ERROR("Connection pool not initialized");
        cel_metric_inc(CEL_M_DB_ACQUIRE_FAIL);
        return NULL;
    }

    pthread_mutex_lock(&g_pool.lock);

    // Wait for an available connection
    while (1) {
        for (int i = 0; i < g_pool.pool_size; i++) {
            if (g_pool.available[i] && g_pool.connections[i]) {
                // Check if connection is still alive
                if (PQstatus(g_pool.connections[i]) == CONNECTION_OK) {
                    g_pool.available[i] = false;
                    pthread_mutex_unlock(&g_pool.lock);
                    LOG_DEBUG("Connection %d acquired", i);
                    return g_pool.connections[i];
                } else {
                    // Connection is dead, try to reconnect
                    LOG_WARN("Connection %d is dead, attempting reconnect", i);
                    PQreset(g_pool.connections[i]);
                    stmt_cache_clear_slot(i);   /* reset wiped the server's prepared statements */

                    if (PQstatus(g_pool.connections[i]) == CONNECTION_OK) {
                        g_pool.available[i] = false;
                        pthread_mutex_unlock(&g_pool.lock);
                        LOG_INFO("Connection %d reconnected successfully", i);
                        return g_pool.connections[i];
                    } else {
                        LOG_ERROR("Failed to reconnect connection %d", i);
                        g_pool.available[i] = false;
                    }
                }
            }
        }

        // No connections available, wait
        LOG_DEBUG("No connections available, waiting...");
        cel_metric_inc(CEL_M_DB_POOL_WAIT);   /* pool saturation signal */
        pthread_cond_wait(&g_pool.cond, &g_pool.lock);
    }

    pthread_mutex_unlock(&g_pool.lock);
    return NULL;
}

void db_connection_release(PGconn *conn) {
    if (!g_pool.initialized || !conn) {
        return;
    }

    pthread_mutex_lock(&g_pool.lock);

    // Find and release the connection
    for (int i = 0; i < g_pool.pool_size; i++) {
        if (g_pool.connections[i] == conn) {
            g_pool.available[i] = true;
            LOG_DEBUG("Connection %d released", i);
            pthread_cond_signal(&g_pool.cond);
            break;
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
}

/* Find-or-prepare `sql` on connection slot `slot`, writing its statement name.
 * Returns 1 (freshly prepared / cache miss), 0 (already cached / hit), or -1 if
 * it could not be prepared. On -1 with prep_err non-NULL, *prep_err receives the
 * failing PGresult (caller owns it); otherwise the failure is cleared here. */
static int cache_prepare(PGconn *conn, int slot, const char *sql, int nparams,
                         char *name_out, size_t cap, PGresult **prep_err) {
    stmt_cache_t *sc = &g_pool.stmts[slot];
    uint64_t h = sql_hash(sql);
    for (int i = 0; i < sc->count; i++)
        if (sc->hash[i] == h && strcmp(sc->sql[i], sql) == 0) {
            snprintf(name_out, cap, "cel_s%d", i);
            return 0;   /* hit */
        }

    /* Cache full → drop the whole set (DEALLOCATE ALL) and start over. Coarse,
     * but with 64 slots an app's distinct query shapes rarely exhaust it. */
    if (sc->count >= STMT_CACHE_MAX) {
        PGresult *d = PQexec(conn, "DEALLOCATE ALL"); PQclear(d);
        stmt_cache_clear_slot(slot);
    }
    int idx = sc->count;
    snprintf(name_out, cap, "cel_s%d", idx);
    PGresult *pr = PQprepare(conn, name_out, sql, nparams, NULL);
    if (PQresultStatus(pr) != PGRES_COMMAND_OK) {
        if (prep_err) *prep_err = pr; else PQclear(pr);
        return -1;
    }
    PQclear(pr);
    char *copy = strdup(sql);
    if (!copy) {        /* OOM: don't leave an untracked statement around */
        PGresult *d = PQexec(conn, "DEALLOCATE ALL"); PQclear(d);
        stmt_cache_clear_slot(slot);
        if (prep_err) *prep_err = NULL;
        return -1;
    }
    sc->hash[idx] = h; sc->sql[idx] = copy; sc->count++;
    return 1;           /* miss, freshly prepared */
}

PGresult *db_connection_exec_cached(PGconn *conn, const char *sql,
                                    int nparams, const char *const *params) {
    if (!conn || !sql) return NULL;

    /* Disabled, or a connection we don't own → plain parameterized exec. */
    int slot;
    if (!g_pool.prepared_enabled || !g_pool.stmts || (slot = slot_of(conn)) < 0)
        return PQexecParams(conn, sql, nparams, NULL, params, NULL, NULL, 0);

    char name[24];
    PGresult *prep_err = NULL;
    int rc = cache_prepare(conn, slot, sql, nparams, name, sizeof name, &prep_err);
    if (rc < 0) {
        if (prep_err) return prep_err;   /* real error (e.g. bad SQL); caller maps it */
        return PQexecParams(conn, sql, nparams, NULL, params, NULL, NULL, 0);  /* OOM */
    }
    cel_metric_inc(rc == 1 ? CEL_M_STMT_PREPARE : CEL_M_STMT_REUSE);

    PGresult *res = PQexecPrepared(conn, name, nparams, params, NULL, NULL, 0);

    /* Self-heal a stale cached plan after DDL ("cached plan must not change
     * result type" → SQLSTATE 0A000): drop the cache so the next call re-prepares
     * against the new schema. This one request still surfaces the error. */
    if (PQresultStatus(res) == PGRES_FATAL_ERROR) {
        const char *ss = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (ss && strcmp(ss, "0A000") == 0) {
            PGresult *d = PQexec(conn, "DEALLOCATE ALL"); PQclear(d);
            stmt_cache_clear_slot(slot);
        }
    }
    return res;
}

int db_connection_prepare_cached(PGconn *conn, const char *sql, int nparams,
                                 char *name_out, size_t cap) {
    if (!conn || !sql || !name_out || cap == 0) return -1;
    int slot;
    if (!g_pool.prepared_enabled || !g_pool.stmts || (slot = slot_of(conn)) < 0)
        return -1;
    int rc = cache_prepare(conn, slot, sql, nparams, name_out, cap, NULL);
    if (rc >= 0) cel_metric_inc(rc == 1 ? CEL_M_STMT_PREPARE : CEL_M_STMT_REUSE);
    return rc;
}

bool db_connection_pipeline_enabled(void) {
    return g_pool.pipeline_enabled;
}

bool db_connection_validate(PGconn *conn) {
    if (!conn) {
        LOG_ERROR("Cannot validate NULL connection");
        return false;
    }

    // Check current connection status
    if (PQstatus(conn) == CONNECTION_OK) {
        // Connection appears OK, but let's verify with a ping
        PGresult *res = PQexec(conn, "SELECT 1");
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            PQclear(res);
            return true;
        }
        PQclear(res);
        LOG_WARN("Connection ping failed, attempting reconnect");
    }

    // Connection is dead or ping failed, attempt reconnection
    LOG_WARN("Connection is dead, attempting reconnect");
    PQreset(conn);
    stmt_cache_clear_slot(slot_of(conn));   /* reset wiped the server's prepared statements */

    if (PQstatus(conn) == CONNECTION_OK) {
        LOG_INFO("Connection successfully reconnected");
        return true;
    }

    LOG_ERROR("Failed to reconnect: %s", PQerrorMessage(conn));
    return false;
}

void db_connection_pool_cleanup(void) {
    if (!g_pool.initialized) {
        return;
    }

    LOG_INFO("Cleaning up database connection pool");

    pthread_mutex_lock(&g_pool.lock);

    // Close all connections
    for (int i = 0; i < g_pool.pool_size; i++) {
        if (g_pool.connections[i]) {
            PQfinish(g_pool.connections[i]);
            g_pool.connections[i] = NULL;
        }
    }

    if (g_pool.stmts) {
        for (int i = 0; i < g_pool.pool_size; i++)
            stmt_cache_clear_slot(i);
        free(g_pool.stmts);
        g_pool.stmts = NULL;
    }

    free(g_pool.connections);
    free(g_pool.available);
    g_pool.connections = NULL;
    g_pool.available = NULL;
    g_pool.pool_size = 0;
    g_pool.initialized = false;

    pthread_mutex_unlock(&g_pool.lock);
    pthread_mutex_destroy(&g_pool.lock);
    pthread_cond_destroy(&g_pool.cond);

    LOG_INFO("Database connection pool cleaned up");
}

bool db_connection_pool_is_initialized(void) {
    return g_pool.initialized;
}

void db_connection_pool_stats(int *size, int *in_use) {
    if (size)   *size   = 0;
    if (in_use) *in_use = 0;
    if (!g_pool.initialized) return;
    pthread_mutex_lock(&g_pool.lock);
    if (size) *size = g_pool.pool_size;
    if (in_use) {
        int busy = 0;
        for (int i = 0; i < g_pool.pool_size; i++)
            if (g_pool.connections[i] && !g_pool.available[i]) busy++;
        *in_use = busy;
    }
    pthread_mutex_unlock(&g_pool.lock);
}
