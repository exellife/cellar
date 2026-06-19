/**
 * @file opcode_dispatcher.h
 * @brief High-Performance Opcode Dispatcher with Computed Goto
 * 
 * Ultra-low latency message routing library using computed goto for 
 * sub-microsecond opcode dispatch. Designed for high-frequency trading,
 * real-time systems, and performance-critical applications.
 *
 * Features:
 * - Computed goto dispatch (2-5 CPU cycles vs 10-20 for switch-case)
 * - Multi-pool thread execution (CPU/IO/DB/INLINE)
 * - Compile-time optimized handler registration
 * - Request/response correlation
 * - Performance monitoring
 * - Memory pool management
 *
 * @author RacyTech Development Team
 * @version 1.0.0
 */

#ifndef OPCODE_DISPATCHER_H
#define OPCODE_DISPATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/time.h>

/* ================================ CONSTANTS ================================ */

#define OPCODE_DISPATCHER_VERSION_MAJOR    1
#define OPCODE_DISPATCHER_VERSION_MINOR    0
#define OPCODE_DISPATCHER_VERSION_PATCH    0

#define OPCODE_DISPATCHER_MAX_OPCODES      65536
#define OPCODE_DISPATCHER_MAX_POOLS        16
#define OPCODE_DISPATCHER_MAX_THREADS      256
#define OPCODE_DISPATCHER_MAX_QUEUE_SIZE   1000000

/* ================================ ENUMS ================================ */

/**
 * @brief Execution pool types for different computational characteristics
 */
typedef enum {
    POOL_INLINE = 0,    /**< Execute immediately on caller thread (<1ms ops) */
    POOL_CPU,           /**< CPU-intensive operations thread pool */
    POOL_IO,            /**< I/O bound operations thread pool */
    POOL_DB,            /**< Database operations thread pool */
    POOL_CUSTOM_START   /**< Custom pool types start from here */
} pool_type_t;

/**
 * @brief Message priority levels
 */
typedef enum {
    PRIORITY_LOW = 0,
    PRIORITY_NORMAL,
    PRIORITY_HIGH,
    PRIORITY_CRITICAL
} priority_t;

/**
 * @brief Handler execution result
 */
typedef enum {
    RESULT_SUCCESS = 0,
    RESULT_ERROR,
    RESULT_ASYNC,       /**< Handler will complete asynchronously */
    RESULT_RETRY        /**< Request should be retried */
} handler_result_t;

/* ================================ FORWARD DECLARATIONS ================================ */

typedef struct opcode_context opcode_context_t;
typedef struct opcode_handler opcode_handler_t;
typedef struct opcode_dispatcher opcode_dispatcher_t;
typedef struct opcode_pool_config opcode_pool_config_t;
typedef struct opcode_stats opcode_stats_t;

/* ================================ TYPES ================================ */

/**
 * @brief Opcode handler function signature
 * 
 * @param ctx Context containing message data and metadata
 * @return handler_result_t Result of handler execution
 */
typedef handler_result_t (*opcode_handler_func_t)(opcode_context_t *ctx);

/**
 * @brief Response callback function signature
 * 
 * @param ctx Original context
 * @param response_data Response payload
 * @param response_size Size of response data
 * @param user_data User-provided callback data
 */
typedef void (*opcode_response_callback_t)(opcode_context_t *ctx, 
                                         const void *response_data, 
                                         size_t response_size, 
                                         void *user_data);

/**
 * @brief Message context containing all request/response data
 */
struct opcode_context {
    uint32_t opcode;                    /**< Operation code */
    uint32_t message_id;                /**< Unique message identifier */
    uint32_t flags;                     /**< Message flags */
    
    const void *data;                   /**< Message payload */
    size_t data_size;                   /**< Payload size */
    
    void *response_data;                /**< Response buffer */
    size_t response_size;               /**< Response data size */
    size_t response_capacity;           /**< Response buffer capacity */
    
    priority_t priority;                /**< Message priority */
    struct timeval timestamp;           /**< Message receive timestamp */
    
    opcode_response_callback_t callback;/**< Response callback */
    void *callback_data;                /**< User callback data */
    
    void *user_data;                    /**< Handler-specific user data */
    bool owns_data;                     /**< Whether context owns the data */
};

/**
 * @brief Opcode handler registration
 */
struct opcode_handler {
    uint32_t opcode;                    /**< Operation code */
    opcode_handler_func_t handler;      /**< Handler function */
    pool_type_t pool_type;              /**< Execution pool */
    void *label_ptr;                    /**< Computed goto label pointer */
    const char *name;                   /**< Handler name for debugging */
    uint64_t call_count;                /**< Number of times called */
    uint64_t total_time_ns;             /**< Total execution time in nanoseconds */
};

/**
 * @brief Thread pool configuration
 */
struct opcode_pool_config {
    pool_type_t pool_type;              /**< Pool type */
    uint32_t thread_count;              /**< Number of threads in pool */
    uint32_t queue_size;                /**< Maximum queue size */
    const char *name;                   /**< Pool name */
};

/**
 * @brief Performance statistics
 */
struct opcode_stats {
    _Atomic uint64_t total_messages;            /**< Total messages processed */
    _Atomic uint64_t successful_messages;       /**< Successfully processed messages */
    _Atomic uint64_t failed_messages;           /**< Failed messages */
    uint64_t avg_dispatch_time_ns;              /**< Average dispatch time (updated periodically) */
    uint64_t avg_execution_time_ns;             /**< Average handler execution time (updated periodically) */
    uint64_t queue_depth[OPCODE_DISPATCHER_MAX_POOLS]; /**< Current queue depths */
};

/* ================================ CORE API ================================ */

/**
 * @brief Create a new opcode dispatcher
 * 
 * @param pool_configs Array of pool configurations
 * @param pool_count Number of pools to create
 * @return opcode_dispatcher_t* New dispatcher instance or NULL on error
 */
opcode_dispatcher_t *opcode_dispatcher_create(const opcode_pool_config_t *pool_configs,
                                             uint32_t pool_count);

/**
 * @brief Destroy dispatcher and cleanup resources
 * 
 * @param dispatcher Dispatcher to destroy
 */
void opcode_dispatcher_destroy(opcode_dispatcher_t *dispatcher);

/**
 * @brief Register an opcode handler
 * 
 * @param dispatcher Target dispatcher
 * @param opcode Operation code to register
 * @param handler Handler function
 * @param pool_type Execution pool for this handler
 * @param name Handler name for debugging
 * @return true on success, false on error
 */
bool opcode_dispatcher_register_handler(opcode_dispatcher_t *dispatcher,
                                       uint32_t opcode,
                                       opcode_handler_func_t handler,
                                       pool_type_t pool_type,
                                       const char *name);

/**
 * @brief Unregister an opcode handler
 * 
 * @param dispatcher Target dispatcher
 * @param opcode Operation code to unregister
 * @return true on success, false if not found
 */
bool opcode_dispatcher_unregister_handler(opcode_dispatcher_t *dispatcher,
                                         uint32_t opcode);

/**
 * @brief Dispatch a message using computed goto (ULTRA FAST)
 * 
 * This is the critical path function designed for maximum performance.
 * Uses computed goto for 2-5 CPU cycle dispatch vs 10-20 for switch-case.
 * 
 * @param dispatcher Target dispatcher
 * @param ctx Message context
 * @return handler_result_t Dispatch result
 */
handler_result_t opcode_dispatcher_dispatch(opcode_dispatcher_t *dispatcher,
                                         opcode_context_t *ctx);

/**
 * @brief Dispatch message synchronously (blocks until completion)
 * 
 * @param dispatcher Target dispatcher
 * @param ctx Message context
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return handler_result_t Final result
 */
handler_result_t opcode_dispatcher_dispatch_sync(opcode_dispatcher_t *dispatcher,
                                               opcode_context_t *ctx,
                                               uint32_t timeout_ms);

/* ================================ CONTEXT MANAGEMENT ================================ */

/**
 * @brief Create a new message context
 * 
 * @param opcode Operation code
 * @param message_id Unique message ID
 * @param data Message payload (can be NULL)
 * @param data_size Payload size
 * @return opcode_context_t* New context or NULL on error
 */
opcode_context_t *opcode_context_create(uint32_t opcode,
                                       uint32_t message_id,
                                       const void *data,
                                       size_t data_size);

/**
 * @brief Destroy a message context
 * 
 * @param ctx Context to destroy
 */
void opcode_context_destroy(opcode_context_t *ctx);

/**
 * @brief Set response data in context
 * 
 * @param ctx Target context
 * @param data Response data
 * @param size Response size
 * @return true on success, false on error
 */
bool opcode_context_set_response(opcode_context_t *ctx,
                                const void *data,
                                size_t size);

/**
 * @brief Set response callback
 * 
 * @param ctx Target context
 * @param callback Callback function
 * @param user_data User data for callback
 */
void opcode_context_set_callback(opcode_context_t *ctx,
                                opcode_response_callback_t callback,
                                void *user_data);

/* ================================ BULK REGISTRATION ================================ */

/**
 * @brief Opcode handler registration descriptor for bulk registration
 */
typedef struct {
    uint32_t opcode;                    /**< Operation code */
    opcode_handler_func_t handler;      /**< Handler function pointer */
    pool_type_t pool_type;              /**< Execution pool type */
    const char *name;                   /**< Handler name (optional) */
} opcode_handler_descriptor_t;

/**
 * @brief Register multiple handlers at once (compile-time optimized)
 * 
 * Example usage:
 *   opcode_handler_descriptor_t handlers[] = {
 *       {0x01, handle_login, POOL_CPU, "login"},
 *       {0x02, handle_logout, POOL_INLINE, "logout"},
 *       {0x03, handle_query, POOL_DB, "query"},
 *   };
 *   opcode_dispatcher_register_bulk(dispatcher, handlers, 3);
 * 
 * @param dispatcher Target dispatcher
 * @param descriptors Array of handler descriptors
 * @param count Number of descriptors
 * @return Number of handlers successfully registered
 */
uint32_t opcode_dispatcher_register_bulk(opcode_dispatcher_t *dispatcher,
                                        const opcode_handler_descriptor_t *descriptors,
                                        uint32_t count);

/* ================================ MONITORING ================================ */

/**
 * @brief Get performance statistics
 * 
 * @param dispatcher Target dispatcher
 * @param stats Output statistics structure
 * @return true on success, false on error
 */
bool opcode_dispatcher_get_stats(opcode_dispatcher_t *dispatcher,
                                opcode_stats_t *stats);

/**
 * @brief Reset performance counters
 * 
 * @param dispatcher Target dispatcher
 */
void opcode_dispatcher_reset_stats(opcode_dispatcher_t *dispatcher);

/**
 * @brief Print performance report to stdout
 * 
 * @param dispatcher Target dispatcher
 */
void opcode_dispatcher_print_stats(opcode_dispatcher_t *dispatcher);

/* ================================ UTILITY FUNCTIONS ================================ */

/**
 * @brief Get library version string
 * 
 * @return const char* Version string
 */
const char *opcode_dispatcher_get_version(void);

/**
 * @brief Check if opcode is registered
 * 
 * @param dispatcher Target dispatcher
 * @param opcode Operation code to check
 * @return true if registered, false otherwise
 */
bool opcode_dispatcher_is_registered(opcode_dispatcher_t *dispatcher,
                                    uint32_t opcode);

/**
 * @brief Get handler info for debugging
 * 
 * @param dispatcher Target dispatcher
 * @param opcode Operation code
 * @return opcode_handler_t* Handler info or NULL if not found
 */
const opcode_handler_t *opcode_dispatcher_get_handler_info(opcode_dispatcher_t *dispatcher,
                                                          uint32_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* OPCODE_DISPATCHER_H */
