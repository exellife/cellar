/* ============================================================================
 * Logger - Simple and Reusable Logging System
 * 
 * Features:
 * - Multiple log levels (DEBUG, INFO, WARN, ERROR)
 * - Compile-time and runtime log level control
 * - File and console output
 * - Thread-safe logging
 * - Timestamped entries
 * - Color-coded console output (optional)
 * - Zero overhead when disabled
 * ============================================================================ */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <string.h>

/* ============================================================================
 * Log Levels
 * ============================================================================ */

typedef enum {
    LOG_LEVEL_DEBUG = 0,    /* Detailed debug information */
    LOG_LEVEL_INFO  = 1,    /* General informational messages */
    LOG_LEVEL_WARN  = 2,    /* Warning messages */
    LOG_LEVEL_ERROR = 3,    /* Error messages */
    LOG_LEVEL_NONE  = 4     /* Disable all logging */
} log_level_t;

/* ============================================================================
 * Logger Configuration
 * ============================================================================ */

typedef struct {
    log_level_t level;              /* Minimum level to log */
    FILE *file;                     /* Log file (NULL for console only) */
    int enable_colors;              /* Enable ANSI colors in console */
    int enable_timestamps;          /* Add timestamps to logs */
    int enable_thread_id;           /* Include thread ID in logs */
    pthread_mutex_t mutex;          /* Thread-safe logging */
} logger_t;

/* ============================================================================
 * Global Logger Instance
 * ============================================================================ */

extern logger_t *g_logger;

/* ============================================================================
 * Logger Functions
 * ============================================================================ */

/**
 * Initialize the logger
 * @param level Minimum log level
 * @param log_file Path to log file (NULL for console only)
 * @param enable_colors Enable color output
 * @return 0 on success, -1 on error
 */
int logger_init(log_level_t level, const char *log_file, int enable_colors);

/**
 * Shutdown and cleanup logger
 */
void logger_shutdown(void);

/**
 * Set the log level at runtime
 * @param level New log level
 */
void logger_set_level(log_level_t level);

/**
 * Get current log level
 * @return Current log level
 */
log_level_t logger_get_level(void);

/**
 * Internal logging function - use macros instead
 */
void logger_log(log_level_t level, const char *file, int line, 
                const char *func, const char *fmt, ...);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#ifdef NDEBUG
    /* Production build - disable DEBUG logs at compile time */
    #define LOG_DEBUG(fmt, ...) ((void)0)
#else
    #define LOG_DEBUG(fmt, ...) \
        logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#endif

#define LOG_INFO(fmt, ...) \
    logger_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    logger_log(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* ============================================================================
 * Shorthand Macros (Optional)
 * ============================================================================ */

#define LOGD LOG_DEBUG
#define LOGI LOG_INFO
#define LOGW LOG_WARN
#define LOGE LOG_ERROR

/* ============================================================================
 * String Conversion
 * ============================================================================ */

/**
 * Convert log level to string
 * @param level Log level
 * @return String representation
 */
const char* logger_level_to_string(log_level_t level);

/**
 * Parse log level from string
 * @param str String representation ("debug", "info", "warn", "error")
 * @return Log level or LOG_LEVEL_INFO if invalid
 */
log_level_t logger_string_to_level(const char *str);

#endif /* LOGGER_H */
