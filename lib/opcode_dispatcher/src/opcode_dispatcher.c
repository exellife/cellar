/**
 * @file opcode_dispatcher.c
 * @brief High-Performance Opcode Dispatcher Implementation
 * 
 * Core implementation of the computed goto opcode dispatcher.
 * Critical path optimized for sub-microsecond performance.
 */

#define _GNU_SOURCE  /* For strdup() */
#include "opcode_dispatcher.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/queue.h>
#include <stdatomic.h>  /* For atomic operations */

/* Define TAILQ_FOREACH_SAFE if not available */
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)                      \
    for ((var) = TAILQ_FIRST((head));                                  \
         (var) && ((tvar) = TAILQ_NEXT((var), field), 1);              \
         (var) = (tvar))
#endif

/* ================================ INTERNAL STRUCTURES ================================ */

/**
 * @brief Work item for thread pools
 */
typedef struct opcode_work_item {
    opcode_context_t *ctx;              /**< Message context */
    opcode_handler_t *handler;          /**< Handler to execute */
    TAILQ_ENTRY(opcode_work_item) entry;/**< Queue linkage */
} opcode_work_item_t;

TAILQ_HEAD(work_queue, opcode_work_item);

/**
 * @brief Thread pool structure
 */
typedef struct opcode_thread_pool {
    pool_type_t pool_type;              /**< Pool type */
    pthread_t *threads;                 /**< Worker threads */
    uint32_t thread_count;              /**< Number of threads */
    
    struct work_queue work_queue;       /**< Work queue */
    pthread_mutex_t queue_mutex;        /**< Queue protection */
    pthread_cond_t queue_cond;          /**< Queue condition variable */
    
    uint32_t queue_size;                /**< Current queue size */
    uint32_t max_queue_size;            /**< Maximum queue size */
    
    bool shutdown;                      /**< Shutdown flag */
    const char *name;                   /**< Pool name */
    
    /* Statistics */
    uint64_t processed_count;           /**< Messages processed */
    uint64_t rejected_count;            /**< Messages rejected (queue full) */
} opcode_thread_pool_t;

/**
 * @brief Main dispatcher structure
 */
struct opcode_dispatcher {
    opcode_handler_t handlers[OPCODE_DISPATCHER_MAX_OPCODES]; /**< Handler table */
    void *goto_table[OPCODE_DISPATCHER_MAX_OPCODES];         /**< Computed goto table */
    
    opcode_thread_pool_t pools[OPCODE_DISPATCHER_MAX_POOLS]; /**< Thread pools */
    uint32_t pool_count;                /**< Number of active pools */
    
    /* Statistics */
    opcode_stats_t stats;               /**< Performance statistics (atomic) */
    
    /* Memory pools for contexts */
    opcode_context_t *context_pool;     /**< Pre-allocated contexts */
    uint32_t context_pool_size;         /**< Pool size */
    uint32_t context_pool_next;         /**< Next available context */
    pthread_mutex_t context_pool_mutex; /**< Context pool protection */
};

/* ================================ STATIC FUNCTIONS ================================ */

static void *thread_pool_worker(void *arg);
static bool thread_pool_init(opcode_thread_pool_t *pool, const opcode_pool_config_t *config);
static void thread_pool_destroy(opcode_thread_pool_t *pool);
static bool thread_pool_enqueue(opcode_thread_pool_t *pool, opcode_context_t *ctx, opcode_handler_t *handler);
static uint64_t get_time_ns(void);
static inline void update_handler_stats(opcode_handler_t *handler, uint64_t execution_time_ns);

/* ================================ COMPUTED GOTO LABELS ================================ */

/* 
 * CRITICAL PERFORMANCE SECTION - COMPUTED GOTO DISPATCH
 * 
 * This is where the magic happens. Instead of using switch-case (10-20 CPU cycles),
 * we use computed goto which resolves in 2-5 CPU cycles for ultra-low latency.
 */

/**
 * @brief The ultra-fast computed goto dispatcher
 * 
 * This function is the heart of the high-performance dispatch system.
 * It uses GCC's computed goto extension for maximum speed.
 */
handler_result_t opcode_dispatcher_dispatch(opcode_dispatcher_t *dispatcher,
                                         opcode_context_t *ctx) {
    if (!dispatcher || !ctx) {
        return RESULT_ERROR;
    }
    
    uint32_t opcode = ctx->opcode;
    
    /* Bounds check */
    if (opcode >= OPCODE_DISPATCHER_MAX_OPCODES) {
        return RESULT_ERROR;
    }
    
    opcode_handler_t *handler = &dispatcher->handlers[opcode];
    
    /* Check if handler is registered */
    if (!handler->handler) {
        return RESULT_ERROR;
    }
    
    /* ULTRA-FAST COMPUTED GOTO DISPATCH */
    static void *pool_dispatch_table[] = {
        &&label_pool_inline,     // POOL_INLINE = 0
        &&label_pool_cpu,        // POOL_CPU = 1
        &&label_pool_io,         // POOL_IO = 2
        &&label_pool_db          // POOL_DB = 3
    };
    
    /* Bounds check for pool type */
    if (handler->pool_type >= sizeof(pool_dispatch_table) / sizeof(pool_dispatch_table[0])) {
        return RESULT_ERROR;
    }
    
    /* Computed goto dispatch - 2-5 CPU cycles instead of 10-20 with switch */
    goto *pool_dispatch_table[handler->pool_type];

label_pool_inline: {
    /* Execute immediately for ultra-fast operations (<1ms) */
    uint64_t exec_start = get_time_ns();
    handler_result_t result = handler->handler(ctx);
    uint64_t exec_time = get_time_ns() - exec_start;
    
    update_handler_stats(handler, exec_time);
    
    /* Update dispatcher stats with atomic operations (no mutex needed) */
    atomic_fetch_add(&dispatcher->stats.total_messages, 1);
    if (result == RESULT_SUCCESS) {
        atomic_fetch_add(&dispatcher->stats.successful_messages, 1);
    } else {
        atomic_fetch_add(&dispatcher->stats.failed_messages, 1);
    }
    
    /* Note: avg_dispatch_time_ns is updated periodically, not per message */
    
    return result;
}

label_pool_cpu:
label_pool_io:
label_pool_db:
    /* Enqueue to appropriate thread pool for background processing */
    atomic_fetch_add(&dispatcher->stats.total_messages, 1);
    
    /* Find the correct pool by pool_type */
    opcode_thread_pool_t *target_pool = NULL;
    for (uint32_t i = 0; i < dispatcher->pool_count; i++) {
        if (dispatcher->pools[i].pool_type == handler->pool_type) {
            target_pool = &dispatcher->pools[i];
            break;
        }
    }
    
    if (target_pool && thread_pool_enqueue(target_pool, ctx, handler)) {
        return RESULT_ASYNC;
    } else {
        atomic_fetch_add(&dispatcher->stats.failed_messages, 1);
        return RESULT_ERROR;
    }
}

/* ================================ CORE API IMPLEMENTATION ================================ */

opcode_dispatcher_t *opcode_dispatcher_create(const opcode_pool_config_t *pool_configs,
                                             uint32_t pool_count) {
    if (!pool_configs || pool_count == 0 || pool_count > OPCODE_DISPATCHER_MAX_POOLS) {
        return NULL;
    }
    
    opcode_dispatcher_t *dispatcher = calloc(1, sizeof(opcode_dispatcher_t));
    if (!dispatcher) {
        return NULL;
    }
    
    /* Initialize mutexes */
    if (pthread_mutex_init(&dispatcher->context_pool_mutex, NULL) != 0) {
        free(dispatcher);
        return NULL;
    }
    
    /* Initialize thread pools */
    dispatcher->pool_count = pool_count;
    for (uint32_t i = 0; i < pool_count; i++) {
        if (!thread_pool_init(&dispatcher->pools[i], &pool_configs[i])) {
            opcode_dispatcher_destroy(dispatcher);
            return NULL;
        }
    }
    
    /* Initialize context pool */
    dispatcher->context_pool_size = 10000; /* Default pool size */
    dispatcher->context_pool = calloc(dispatcher->context_pool_size, 
                                    sizeof(opcode_context_t));
    if (!dispatcher->context_pool) {
        opcode_dispatcher_destroy(dispatcher);
        return NULL;
    }
    
    /* Initialize computed goto table with NULL pointers */
    memset(dispatcher->goto_table, 0, sizeof(dispatcher->goto_table));
    
    return dispatcher;
}

void opcode_dispatcher_destroy(opcode_dispatcher_t *dispatcher) {
    if (!dispatcher) {
        return;
    }
    
    /* Shutdown thread pools */
    for (uint32_t i = 0; i < dispatcher->pool_count; i++) {
        thread_pool_destroy(&dispatcher->pools[i]);
    }

    /* Free strdup'd handler names */
    for (uint32_t i = 0; i < OPCODE_DISPATCHER_MAX_OPCODES; i++) {
        free((void *)dispatcher->handlers[i].name);
        dispatcher->handlers[i].name = NULL;
    }

    /* Cleanup context pool */
    free(dispatcher->context_pool);
    
    /* Destroy mutexes */
    pthread_mutex_destroy(&dispatcher->context_pool_mutex);
    
    free(dispatcher);
}

bool opcode_dispatcher_register_handler(opcode_dispatcher_t *dispatcher,
                                       uint32_t opcode,
                                       opcode_handler_func_t handler,
                                       pool_type_t pool_type,
                                       const char *name) {
    if (!dispatcher || !handler || opcode >= OPCODE_DISPATCHER_MAX_OPCODES) {
        return false;
    }
    
    opcode_handler_t *handler_entry = &dispatcher->handlers[opcode];
    
    /* Check if already registered */
    if (handler_entry->handler) {
        return false; /* Already registered */
    }
    
    /* Register handler */
    handler_entry->opcode = opcode;
    handler_entry->handler = handler;
    handler_entry->pool_type = pool_type;
    handler_entry->name = name ? strdup(name) : NULL;
    handler_entry->call_count = 0;
    handler_entry->total_time_ns = 0;
    
    /* Set label pointer to NULL initially - will be set during dispatch initialization */
    handler_entry->label_ptr = NULL;
    
    return true;
}

uint32_t opcode_dispatcher_register_bulk(opcode_dispatcher_t *dispatcher,
                                        const opcode_handler_descriptor_t *descriptors,
                                        uint32_t count) {
    if (!dispatcher || !descriptors) {
        return 0;
    }
    
    uint32_t registered = 0;
    
    /* Register each handler in the descriptor array */
    for (uint32_t i = 0; i < count; i++) {
        const opcode_handler_descriptor_t *desc = &descriptors[i];
        
        if (opcode_dispatcher_register_handler(dispatcher,
                                              desc->opcode,
                                              desc->handler,
                                              desc->pool_type,
                                              desc->name)) {
            registered++;
        }
    }
    
    return registered;
}

bool opcode_dispatcher_unregister_handler(opcode_dispatcher_t *dispatcher,
                                         uint32_t opcode) {
    if (!dispatcher || opcode >= OPCODE_DISPATCHER_MAX_OPCODES) {
        return false;
    }
    
    opcode_handler_t *handler = &dispatcher->handlers[opcode];
    
    if (!handler->handler) {
        return false; /* Not registered */
    }
    
    /* Cleanup */
    free((void*)handler->name);
    memset(handler, 0, sizeof(opcode_handler_t));
    dispatcher->goto_table[opcode] = NULL;
    
    return true;
}

handler_result_t opcode_dispatcher_dispatch_sync(opcode_dispatcher_t *dispatcher,
                                               opcode_context_t *ctx,
                                               uint32_t timeout_ms) {
    (void)timeout_ms; /* TODO: Implement timeout handling */
    /* For sync dispatch, we force inline execution regardless of pool type */
    if (!dispatcher || !ctx) {
        return RESULT_ERROR;
    }
    
    uint32_t opcode = ctx->opcode;
    if (opcode >= OPCODE_DISPATCHER_MAX_OPCODES) {
        return RESULT_ERROR;
    }
    
    opcode_handler_t *handler = &dispatcher->handlers[opcode];
    if (!handler->handler) {
        return RESULT_ERROR;
    }
    
    /* Execute directly */
    uint64_t start = get_time_ns();
    handler_result_t result = handler->handler(ctx);
    uint64_t exec_time = get_time_ns() - start;
    
    update_handler_stats(handler, exec_time);
    
    return result;
}

/* ================================ CONTEXT MANAGEMENT ================================ */

opcode_context_t *opcode_context_create(uint32_t opcode,
                                       uint32_t message_id,
                                       const void *data,
                                       size_t data_size) {
    opcode_context_t *ctx = calloc(1, sizeof(opcode_context_t));
    if (!ctx) {
        return NULL;
    }
    
    ctx->opcode = opcode;
    ctx->message_id = message_id;
    ctx->priority = PRIORITY_NORMAL;
    gettimeofday(&ctx->timestamp, NULL);
    
    if (data && data_size > 0) {
        void *data_copy = malloc(data_size);
        if (!data_copy) {
            free(ctx);
            return NULL;
        }
        memcpy(data_copy, data, data_size);
        ctx->data = data_copy;
        ctx->data_size = data_size;
        ctx->owns_data = true;
    }
    
    return ctx;
}

void opcode_context_destroy(opcode_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->owns_data) {
        free((void*)ctx->data);
        free(ctx->response_data);
    }
    
    free(ctx);
}

bool opcode_context_set_response(opcode_context_t *ctx,
                                const void *data,
                                size_t size) {
    if (!ctx) {
        return false;
    }
    
    if (ctx->response_capacity < size) {
        void *new_buffer = realloc(ctx->response_data, size);
        if (!new_buffer) {
            return false;
        }
        ctx->response_data = new_buffer;
        ctx->response_capacity = size;
    }
    
    if (data && size > 0) {
        memcpy(ctx->response_data, data, size);
    }
    ctx->response_size = size;
    
    return true;
}

void opcode_context_set_callback(opcode_context_t *ctx,
                                opcode_response_callback_t callback,
                                void *user_data) {
    if (ctx) {
        ctx->callback = callback;
        ctx->callback_data = user_data;
    }
}

/* ================================ THREAD POOL IMPLEMENTATION ================================ */

static bool thread_pool_init(opcode_thread_pool_t *pool, const opcode_pool_config_t *config) {
    if (!pool || !config) {
        return false;
    }
    
    /* Allow thread_count = 0 for POOL_INLINE */
    if (config->pool_type != POOL_INLINE && config->thread_count == 0) {
        return false;
    }
    
    pool->pool_type = config->pool_type;
    pool->thread_count = config->thread_count;
    pool->max_queue_size = config->queue_size;
    pool->name = config->name ? strdup(config->name) : NULL;
    pool->shutdown = false;
    
    /* Initialize queue */
    TAILQ_INIT(&pool->work_queue);
    
    /* Initialize synchronization */
    if (pthread_mutex_init(&pool->queue_mutex, NULL) != 0 ||
        pthread_cond_init(&pool->queue_cond, NULL) != 0) {
        return false;
    }
    
    /* Create worker threads (skip for inline pools) */
    if (pool->thread_count > 0) {
        pool->threads = calloc(pool->thread_count, sizeof(pthread_t));
        if (!pool->threads) {
            pthread_mutex_destroy(&pool->queue_mutex);
            pthread_cond_destroy(&pool->queue_cond);
            return false;
        }
        
        for (uint32_t i = 0; i < pool->thread_count; i++) {
            if (pthread_create(&pool->threads[i], NULL, thread_pool_worker, pool) != 0) {
                /* Cleanup on failure */
                pool->shutdown = true;
                pthread_cond_broadcast(&pool->queue_cond);
                
                for (uint32_t j = 0; j < i; j++) {
                    pthread_join(pool->threads[j], NULL);
                }
                
                free(pool->threads);
                pthread_mutex_destroy(&pool->queue_mutex);
                pthread_cond_destroy(&pool->queue_cond);
                return false;
            }
        }
    } else {
        /* Inline pool - no threads needed */
        pool->threads = NULL;
    }
    
    return true;
}

static void thread_pool_destroy(opcode_thread_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    /* Signal shutdown */
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
    
    /* Wait for threads to finish */
    if (pool->threads) {
        for (uint32_t i = 0; i < pool->thread_count; i++) {
            pthread_join(pool->threads[i], NULL);
        }
        free(pool->threads);
    }
    
    /* Cleanup remaining work items */
    opcode_work_item_t *item, *tmp;
    TAILQ_FOREACH_SAFE(item, &pool->work_queue, entry, tmp) {
        TAILQ_REMOVE(&pool->work_queue, item, entry);
        free(item);
    }
    
    /* Destroy synchronization */
    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    
    /* Cleanup name */
    free((void*)pool->name);
}

static bool thread_pool_enqueue(opcode_thread_pool_t *pool, 
                               opcode_context_t *ctx, 
                               opcode_handler_t *handler) {
    if (!pool || !ctx || !handler) {
        return false;
    }
    
    opcode_work_item_t *item = malloc(sizeof(opcode_work_item_t));
    if (!item) {
        return false;
    }
    
    item->ctx = ctx;
    item->handler = handler;
    
    pthread_mutex_lock(&pool->queue_mutex);
    
    /* Check queue capacity */
    if (pool->queue_size >= pool->max_queue_size) {
        pthread_mutex_unlock(&pool->queue_mutex);
        free(item);
        pool->rejected_count++;
        return false;
    }
    
    /* Enqueue item */
    TAILQ_INSERT_TAIL(&pool->work_queue, item, entry);
    pool->queue_size++;
    
    /* Signal worker */
    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
    
    return true;
}

static void *thread_pool_worker(void *arg) {
    opcode_thread_pool_t *pool = (opcode_thread_pool_t *)arg;
    
    while (true) {
        pthread_mutex_lock(&pool->queue_mutex);
        
        /* Wait for work or shutdown */
        while (TAILQ_EMPTY(&pool->work_queue) && !pool->shutdown) {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }
        
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }
        
        /* Get work item */
        opcode_work_item_t *item = TAILQ_FIRST(&pool->work_queue);
        TAILQ_REMOVE(&pool->work_queue, item, entry);
        pool->queue_size--;
        
        pthread_mutex_unlock(&pool->queue_mutex);
        
        /* Execute handler */
        if (item && item->handler && item->ctx) {
            uint64_t start = get_time_ns();
            handler_result_t result = item->handler->handler(item->ctx);
            uint64_t exec_time = get_time_ns() - start;
            
            (void)result; /* TODO: Handle result for async operations */
            update_handler_stats(item->handler, exec_time);
            pool->processed_count++;
            
            /* Call response callback if set */
            if (item->ctx->callback) {
                item->ctx->callback(item->ctx, 
                                   item->ctx->response_data,
                                   item->ctx->response_size,
                                   item->ctx->callback_data);
            }
        }
        
        free(item);
    }
    
    return NULL;
}

/* ================================ UTILITY FUNCTIONS ================================ */

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void update_handler_stats(opcode_handler_t *handler, uint64_t execution_time_ns) {
    if (handler) {
        __atomic_add_fetch(&handler->call_count, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&handler->total_time_ns, execution_time_ns, __ATOMIC_RELAXED);
    }
}

const char *opcode_dispatcher_get_version(void) {
    return "1.0.0";
}

bool opcode_dispatcher_is_registered(opcode_dispatcher_t *dispatcher,
                                    uint32_t opcode) {
    if (!dispatcher || opcode >= OPCODE_DISPATCHER_MAX_OPCODES) {
        return false;
    }
    
    return dispatcher->handlers[opcode].handler != NULL;
}

const opcode_handler_t *opcode_dispatcher_get_handler_info(opcode_dispatcher_t *dispatcher,
                                                          uint32_t opcode) {
    if (!dispatcher || opcode >= OPCODE_DISPATCHER_MAX_OPCODES) {
        return NULL;
    }
    
    if (dispatcher->handlers[opcode].handler) {
        return &dispatcher->handlers[opcode];
    }
    
    return NULL;
}

/* ================================ STATISTICS FUNCTIONS ================================ */

bool opcode_dispatcher_get_stats(opcode_dispatcher_t *dispatcher,
                                opcode_stats_t *stats) {
    if (!dispatcher || !stats) {
        return false;
    }
    
    /* Read atomic stats without locking */
    stats->total_messages = atomic_load(&dispatcher->stats.total_messages);
    stats->successful_messages = atomic_load(&dispatcher->stats.successful_messages);
    stats->failed_messages = atomic_load(&dispatcher->stats.failed_messages);
    
    /* Copy non-atomic fields that are updated periodically */
    stats->avg_dispatch_time_ns = dispatcher->stats.avg_dispatch_time_ns;
    stats->avg_execution_time_ns = dispatcher->stats.avg_execution_time_ns;
    memcpy(stats->queue_depth, dispatcher->stats.queue_depth, sizeof(stats->queue_depth));
    
    return true;
}

void opcode_dispatcher_reset_stats(opcode_dispatcher_t *dispatcher) {
    if (!dispatcher) {
        return;
    }
    
    /* Reset atomic stats */
    atomic_store(&dispatcher->stats.total_messages, 0);
    atomic_store(&dispatcher->stats.successful_messages, 0);
    atomic_store(&dispatcher->stats.failed_messages, 0);
    
    /* Reset non-atomic fields */
    dispatcher->stats.avg_dispatch_time_ns = 0;
    dispatcher->stats.avg_execution_time_ns = 0;
    memset(dispatcher->stats.queue_depth, 0, sizeof(dispatcher->stats.queue_depth));
    
    /* Reset handler stats */
    for (uint32_t i = 0; i < OPCODE_DISPATCHER_MAX_OPCODES; i++) {
        if (dispatcher->handlers[i].handler) {
            __atomic_store_n(&dispatcher->handlers[i].call_count, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&dispatcher->handlers[i].total_time_ns, 0, __ATOMIC_RELAXED);
        }
    }
}

void opcode_dispatcher_print_stats(opcode_dispatcher_t *dispatcher) {
    if (!dispatcher) {
        return;
    }
    
    opcode_stats_t stats;
    if (!opcode_dispatcher_get_stats(dispatcher, &stats)) {
        printf("Failed to get statistics\n");
        return;
    }
    
    printf("\n" 
           "======================== OPCODE DISPATCHER STATISTICS ========================\n"
           "Total Messages:         %lu\n"
           "Successful Messages:    %lu\n"
           "Failed Messages:        %lu\n"
           "Success Rate:           %.2f%%\n"
           "Avg Dispatch Time:      %lu ns\n"
           "Avg Execution Time:     %lu ns\n",
           stats.total_messages,
           stats.successful_messages,
           stats.failed_messages,
           stats.total_messages > 0 ? 
           (100.0 * stats.successful_messages / stats.total_messages) : 0.0,
           stats.avg_dispatch_time_ns,
           stats.avg_execution_time_ns);
    
    printf("\n"
           "========================= HANDLER STATISTICS =========================\n");
    
    for (uint32_t i = 0; i < OPCODE_DISPATCHER_MAX_OPCODES; i++) {
        if (dispatcher->handlers[i].handler) {
            uint64_t calls = __atomic_load_n(&dispatcher->handlers[i].call_count, __ATOMIC_RELAXED);
            uint64_t total_time = __atomic_load_n(&dispatcher->handlers[i].total_time_ns, __ATOMIC_RELAXED);
            uint64_t avg_time = calls > 0 ? total_time / calls : 0;
            
            printf("Opcode 0x%02X (%s): %lu calls, avg %lu ns\n",
                   i, 
                   dispatcher->handlers[i].name ? dispatcher->handlers[i].name : "unnamed",
                   calls,
                   avg_time);
        }
    }
    printf("===============================================================\n\n");
}
