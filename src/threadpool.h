#ifndef THREADPOOL_H
#define THREADPOOL_H

/*
 * Fixed-size worker pool with a bounded FIFO queue of client sockets.
 *
 * Main thread:  accept() -> thread_pool_submit()
 * Workers:      wait on a condition variable, take a socket,
 *               call server_handle_client(), close the socket.
 */
typedef struct thread_pool thread_pool;

/*
 * Create the pool and start 'workers' threads with a queue of
 * 'capacity' slots. Returns NULL on failure (nothing is leaked).
 */
thread_pool *thread_pool_create(int workers, int capacity);

/*
 * Hand a client socket to the pool.
 * Returns 0 on success (a worker now owns the socket).
 * Returns -1 if the queue is full or the pool is shutting down;
 * the CALLER must close the socket in that case.
 */
int thread_pool_submit(thread_pool *pool, int client_fd);

/*
 * Shut the pool down: wake all workers, let them drain queued jobs,
 * join every thread, then free all memory and destroy the mutex and
 * condition variable. Safe to call once.
 */
void thread_pool_destroy(thread_pool *pool);

#endif
