#define _DEFAULT_SOURCE

#include "threadpool.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

struct thread_pool
{
    int *queue;         /* ring buffer of client sockets */
    int capacity;
    int head;           /* index of the next socket to take */
    int tail;           /* index where the next socket is placed */
    int count;          /* number of sockets currently queued */
    int shutdown;       /* set to 1 when the pool should stop */

    pthread_t *threads;
    int num_threads;

    pthread_mutex_t mutex;          /* protects everything above */
    pthread_cond_t work_available;  /* workers sleep here when empty */
};

/* Worker thread body: wait for a socket, handle it, repeat. */
static void *worker_main(void *arg)
{
    thread_pool *pool = (thread_pool *)arg;

    for (;;)
    {
        pthread_mutex_lock(&pool->mutex);

        while (pool->count == 0 && pool->shutdown == 0)
        {
            /* Atomically release the mutex and sleep.
             * Woken by submit() (signal) or shutdown (broadcast). */
            pthread_cond_wait(&pool->work_available, &pool->mutex);
        }

        /* Shutdown with an empty queue: time to exit. */
        if (pool->count == 0 && pool->shutdown == 1)
        {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* Take one socket from the queue. */
        int client_fd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;

        pthread_mutex_unlock(&pool->mutex);

        /* Request data lives only on this worker's stack - no sharing. */
        server_handle_client(client_fd);
        close(client_fd);
    }

    return NULL;
}

thread_pool *thread_pool_create(int workers, int capacity)
{
    if (workers < 1 || capacity < 1)
        return NULL;

    thread_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;

    pool->queue = calloc((size_t)capacity, sizeof(int));
    pool->threads = calloc((size_t)workers, sizeof(pthread_t));

    if (pool->queue == NULL || pool->threads == NULL)
    {
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    pool->capacity = capacity;
    pool->num_threads = workers;

    if (pthread_mutex_init(&pool->mutex, NULL) != 0)
    {
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    if (pthread_cond_init(&pool->work_available, NULL) != 0)
    {
        pthread_mutex_destroy(&pool->mutex);
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    /* Start the workers. On partial failure, stop the ones that
     * started, join them, and free everything. */
    for (int i = 0; i < workers; i++)
    {
        if (pthread_create(&pool->threads[i], NULL,
                           worker_main, pool) != 0)
        {
            fprintf(stderr, "thread_pool: failed to create worker %d\n", i);

            pthread_mutex_lock(&pool->mutex);
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work_available);
            pthread_mutex_unlock(&pool->mutex);

            for (int j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);

            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->queue);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

int thread_pool_submit(thread_pool *pool, int client_fd)
{
    if (pool == NULL)
        return -1;

    pthread_mutex_lock(&pool->mutex);

    if (pool->shutdown != 0 || pool->count == pool->capacity)
    {
        /* Queue full or shutting down: caller closes the socket. */
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->queue[pool->tail] = client_fd;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;

    pthread_cond_signal(&pool->work_available);

    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void thread_pool_destroy(thread_pool *pool)
{
    if (pool == NULL)
        return;

    /* Policy: queued jobs are DRAINED - workers finish everything
     * already accepted, then exit when the queue is empty. */
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_available); /* wake all sleepers */
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->num_threads; i++)
        pthread_join(pool->threads[i], NULL);

    /* No thread touches these anymore: safe to destroy and free. */
    pthread_cond_destroy(&pool->work_available);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->queue);
    free(pool->threads);
    free(pool);
}
