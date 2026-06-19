/* ============================================================================
 * Logger Implementation
 * ============================================================================ */

#include "../include/logger.h"
#include <stdlib.h>
#include <unistd.h>

/* ============================================================================
 * ANSI Color Codes
 * ============================================================================ */

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_COLOR_BOLD    "\x1b[1m"

/* ============================================================================
 * Global Logger
 * ============================================================================ */

logger_t *g_logger = NULL;

/* ============================================================================
 * Level String Conversion
 * ============================================================================ */

const char* logger_level_to_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_NONE:  return "NONE";
        default:              return "UNKNOWN";
    }
}

log_level_t logger_string_to_level(const char *str) {
    if (strcasecmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(str, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(str, "warn") == 0)  return LOG_LEVEL_WARN;
    if (strcasecmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(str, "none") == 0)  return LOG_LEVEL_NONE;
    return LOG_LEVEL_INFO; /* Default */
}

/* ============================================================================
 * Color Helper
 * ============================================================================ */

static const char* get_level_color(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return ANSI_COLOR_CYAN;
        case LOG_LEVEL_INFO:  return ANSI_COLOR_GREEN;
        case LOG_LEVEL_WARN:  return ANSI_COLOR_YELLOW;
        case LOG_LEVEL_ERROR: return ANSI_COLOR_RED;
        default:              return ANSI_COLOR_RESET;
    }
}

/* ============================================================================
 * Logger Initialization
 * ============================================================================ */

int logger_init(log_level_t level, const char *log_file, int enable_colors) {
    if (g_logger != NULL) {
        fprintf(stderr, "Logger already initialized\n");
        return -1;
    }
    
    g_logger = (logger_t *)malloc(sizeof(logger_t));
    if (!g_logger) {
        fprintf(stderr, "Failed to allocate logger\n");
        return -1;
    }
    
    g_logger->level = level;
    g_logger->file = NULL;
    g_logger->enable_colors = enable_colors;
    g_logger->enable_timestamps = 1;
    g_logger->enable_thread_id = 0;
    
    /* Initialize mutex */
    if (pthread_mutex_init(&g_logger->mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize logger mutex\n");
        free(g_logger);
        g_logger = NULL;
        return -1;
    }
    
    /* Open log file if specified */
    if (log_file != NULL) {
        g_logger->file = fopen(log_file, "a");
        if (!g_logger->file) {
            fprintf(stderr, "Failed to open log file: %s\n", log_file);
            pthread_mutex_destroy(&g_logger->mutex);
            free(g_logger);
            g_logger = NULL;
            return -1;
        }
    }
    
    return 0;
}

/* ============================================================================
 * Logger Shutdown
 * ============================================================================ */

void logger_shutdown(void) {
    if (g_logger == NULL) {
        return;
    }
    
    pthread_mutex_lock(&g_logger->mutex);
    
    if (g_logger->file != NULL) {
        fclose(g_logger->file);
        g_logger->file = NULL;
    }
    
    pthread_mutex_unlock(&g_logger->mutex);
    pthread_mutex_destroy(&g_logger->mutex);
    
    free(g_logger);
    g_logger = NULL;
}

/* ============================================================================
 * Level Management
 * ============================================================================ */

void logger_set_level(log_level_t level) {
    if (g_logger == NULL) return;
    
    pthread_mutex_lock(&g_logger->mutex);
    g_logger->level = level;
    pthread_mutex_unlock(&g_logger->mutex);
}

log_level_t logger_get_level(void) {
    if (g_logger == NULL) return LOG_LEVEL_INFO;
    
    pthread_mutex_lock(&g_logger->mutex);
    log_level_t level = g_logger->level;
    pthread_mutex_unlock(&g_logger->mutex);
    
    return level;
}

/* ============================================================================
 * Core Logging Function
 * ============================================================================ */

void logger_log(log_level_t level, const char *file, int line, 
                const char *func, const char *fmt, ...) {
    /* Early exit if logger not initialized or level too low */
    if (g_logger == NULL || level < g_logger->level) {
        return;
    }
    
    pthread_mutex_lock(&g_logger->mutex);
    
    /* Prepare timestamp */
    char timestamp[64] = "";
    if (g_logger->enable_timestamps) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    }
    
    /* Extract just the filename from full path */
    const char *filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;
    
    /* Prepare the log message */
    va_list args;
    va_start(args, fmt);
    
    /* Log to console (stderr for errors, stdout for others) */
    FILE *console = (level >= LOG_LEVEL_ERROR) ? stderr : stdout;
    
    /* Apply colors if enabled and outputting to terminal */
    int use_colors = g_logger->enable_colors && isatty(fileno(console));
    
    if (use_colors) {
        fprintf(console, "%s%s[%s]%s ", 
                get_level_color(level), ANSI_COLOR_BOLD,
                logger_level_to_string(level), ANSI_COLOR_RESET);
    } else {
        fprintf(console, "[%s] ", logger_level_to_string(level));
    }
    
    if (g_logger->enable_timestamps) {
        fprintf(console, "[%s] ", timestamp);
    }
    
    fprintf(console, "[%s:%d:%s] ", filename, line, func);
    vfprintf(console, fmt, args);
    fprintf(console, "\n");
    
    /* Log to file if enabled */
    if (g_logger->file != NULL) {
        va_end(args);
        va_start(args, fmt);
        
        fprintf(g_logger->file, "[%s] ", logger_level_to_string(level));
        
        if (g_logger->enable_timestamps) {
            fprintf(g_logger->file, "[%s] ", timestamp);
        }
        
        fprintf(g_logger->file, "[%s:%d:%s] ", filename, line, func);
        vfprintf(g_logger->file, fmt, args);
        fprintf(g_logger->file, "\n");
        fflush(g_logger->file);
    }
    
    va_end(args);
    pthread_mutex_unlock(&g_logger->mutex);
}
