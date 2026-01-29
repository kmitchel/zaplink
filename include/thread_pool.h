#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stddef.h>

/* Opaque thread pool structure */
typedef struct thread_pool thread_pool_t;

/* Function pointer type for jobs */
typedef void (*thread_func_t)(void *arg);

/**
 * @brief Create a new thread pool.
 * 
 * @param num_threads Number of worker threads to create.
 * @return thread_pool_t* Pointer to the thread pool, or NULL on failure.
 */
thread_pool_t *thread_pool_create(int num_threads);

/**
 * @brief Submit a job to the thread pool.
 * 
 * @param pool Thread pool instance.
 * @param func Function to execute.
 * @param arg Argument to pass to the function.
 * @return int 0 on success, -1 on failure.
 */
int thread_pool_submit(thread_pool_t *pool, thread_func_t func, void *arg);

/**
 * @brief Destroy the thread pool.
 * Waits for current jobs to finish but stops accepting new ones? 
 * Or cancels everything? 
 * Standard shutdown: set shutdown flag, broadcast cond, join threads.
 * 
 * @param pool Thread pool instance.
 */
void thread_pool_destroy(thread_pool_t *pool);

#endif
