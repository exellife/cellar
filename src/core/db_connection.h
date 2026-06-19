/**
 * @file db_connection.h
 * @brief PostgreSQL Database Connection Management
 * 
 * Provides a simple connection pool for database operations.
 * Thread-safe connection management for use by handlers.
 */

#ifndef DB_CONNECTION_H
#define DB_CONNECTION_H

#include <libpq-fe.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initialize database connection pool
 * 
 * Creates a pool of persistent PostgreSQL connections with TCP keepalive
 * enabled to prevent idle connection timeouts.
 * 
 * Keepalive settings:
 * - keepalives=1: Enable TCP keepalive
 * - keepalives_idle=30: Start sending keepalive after 30s idle
 * - keepalives_interval=10: Send keepalive every 10s
 * - keepalives_count=5: Consider dead after 5 failed keepalives
 * 
 * @param conninfo PostgreSQL connection string (with keepalive params)
 * @param pool_size Number of connections in pool
 * @return 0 on success, -1 on error
 */
int db_connection_pool_init(const char *conninfo, int pool_size);

/**
 * @brief Get a connection from the pool
 * 
 * Blocks until a connection is available. Automatically validates
 * connection health and reconnects if necessary.
 * 
 * @return PGconn* Active connection or NULL on error
 */
PGconn *db_connection_acquire(void);

/**
 * @brief Return a connection to the pool
 * 
 * @param conn Connection to release
 */
void db_connection_release(PGconn *conn);

/**
 * @brief Execute a parameterized query, caching its plan as a per-connection
 *        prepared statement keyed by SQL text.
 *
 * Drop-in replacement for `PQexecParams(conn, sql, nparams, NULL, params,
 * NULL, NULL, 0)` (text-format params, text-format results). On the first sight
 * of a given SQL string on a given pooled connection it PQprepares it; every
 * subsequent call PQexecPrepares — eliminating the repeated query planning that
 * profiling showed dominates execution. The cache is cleared automatically when
 * a connection is reset, and self-heals stale plans after DDL.
 *
 * Set CEL_PREPARED_STATEMENTS=0 to disable (falls back to plain PQexecParams).
 * The caller owns the returned PGresult and must PQclear it.
 *
 * @param conn    A connection obtained from db_connection_acquire().
 * @param sql     Parameterized SQL ($1, $2, ...).
 * @param nparams Number of parameters.
 * @param params  Array of text parameter values (may contain NULLs).
 * @return PGresult* (may be an error result), or NULL on bad arguments.
 */
PGresult *db_connection_exec_cached(PGconn *conn, const char *sql,
                                    int nparams, const char *const *params);

/**
 * @brief Ensure `sql` is prepared on `conn` (cache-keyed by text) and return its
 *        statement name — for callers that drive the extended protocol directly
 *        (e.g. PQsendQueryPrepared in pipeline mode).
 *
 * @param conn     A connection from db_connection_acquire().
 * @param sql      Parameterized SQL.
 * @param nparams  Number of parameters.
 * @param name_out Buffer receiving the prepared-statement name on success.
 * @param cap      Size of name_out.
 * @return 1 if freshly prepared, 0 if already cached, -1 if not cached (caching
 *         disabled, foreign connection, or prepare failed → caller should fall
 *         back to a non-prepared path).
 */
int db_connection_prepare_cached(PGconn *conn, const char *sql, int nparams,
                                 char *name_out, size_t cap);

/**
 * @brief Whether pooled-mode pipelining is enabled (CEL_DB_PIPELINE=1 and the
 *        libpq build supports it). When true, the data-API path may send the
 *        BEGIN/set_config/query/COMMIT transaction as a single pipelined round trip.
 */
bool db_connection_pipeline_enabled(void);

/**
 * @brief Validate and repair a connection if needed
 * 
 * Checks if connection is alive and attempts reconnection if dead.
 * Useful for long-running transactions or after errors.
 * 
 * @param conn Connection to validate
 * @return true if connection is OK (or successfully reconnected), false otherwise
 */
bool db_connection_validate(PGconn *conn);

/**
 * @brief Cleanup and close all connections
 */
void db_connection_pool_cleanup(void);

/**
 * @brief Check if pool is initialized
 *
 * @return true if initialized, false otherwise
 */
bool db_connection_pool_is_initialized(void);

/**
 * @brief Sample pool utilization (for metrics).
 *
 * Writes the configured pool size and the number of connections currently
 * checked out. Either pointer may be NULL. Reports 0/0 when uninitialized.
 */
void db_connection_pool_stats(int *size, int *in_use);

#endif /* DB_CONNECTION_H */
