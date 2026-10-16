#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "files.h"
#include "threadpool.h"

#define DEFAULT_PORT    8080
#define DEFAULT_WORKERS 4
#define DEFAULT_QUEUE   32
#define LISTEN_BACKLOG  128

/* Set to 1 by the signal handler; checked by the accept loop. */
static volatile sig_atomic_t g_shutdown = 0;

/*
 * Async-signal-safe handler: it ONLY sets a flag.
 * No printf, no malloc, no locking here - those are not safe inside
 * a signal handler.
 */
static void handle_signal(int signo)
{
    (void)signo;
    g_shutdown = 1;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p port] [-t workers] [-q queue]\n"
            "  -p port     listen port          (default %d)\n"
            "  -t workers  worker threads       (default %d)\n"
            "  -q queue    job queue capacity   (default %d)\n",
            prog, DEFAULT_PORT, DEFAULT_WORKERS, DEFAULT_QUEUE);
}

/*
 * Strict integer parser. Unlike atoi(), rejects trailing garbage
 * ("80abc"), empty strings, and out-of-range values (UB-free via
 * strtol with errno checking).
 * Returns 0 on success, -1 on any invalid input.
 */
static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0')
        return -1;

    errno = 0;
    v = strtol(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < INT_MIN || v > INT_MAX)
        return -1;

    *out = (int)v;
    return 0;
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int workers = DEFAULT_WORKERS;
    int queue_cap = DEFAULT_QUEUE;

    int opt;
    while ((opt = getopt(argc, argv, "p:t:q:h")) != -1)
    {
        int parsed;

        switch (opt)
        {
            case 'p':
                if (parse_int(optarg, &parsed) < 0)
                {
                    fprintf(stderr, "Invalid port: '%s'\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                port = parsed;
                break;
            case 't':
                if (parse_int(optarg, &parsed) < 0)
                {
                    fprintf(stderr, "Invalid worker count: '%s'\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                workers = parsed;
                break;
            case 'q':
                if (parse_int(optarg, &parsed) < 0)
                {
                    fprintf(stderr, "Invalid queue size: '%s'\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                queue_cap = parsed;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Unexpected argument: '%s'\n", argv[optind]);
        print_usage(argv[0]);
        return 1;
    }

    if (port < 1 || port > 65535 ||
        workers < 1 || workers > 128 ||
        queue_cap < 1 || queue_cap > 4096)
    {
        fprintf(stderr,
                "Invalid configuration: port 1-65535, "
                "workers 1-128, queue 1-4096\n");
        print_usage(argv[0]);
        return 1;
    }

    /*
     * SIGINT / SIGTERM: record the request only.
     * sa_flags = 0 (no SA_RESTART) on purpose: the blocking accept()
     * below will return EINTR when the signal arrives, the loop sees
     * g_shutdown, and we exit cleanly.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0)
    {
        perror("sigaction");
        return 1;
    }

    /* Web root directory fd: opened once, read-only from then on. */
    if (files_init(FILES_DEFAULT_ROOT) < 0)
    {
        perror("files_init www");
        return 1;
    }

    thread_pool *pool = thread_pool_create(workers, queue_cap);
    if (pool == NULL)
    {
        fprintf(stderr, "Failed to create thread pool\n");
        files_cleanup();
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        thread_pool_destroy(pool);
        files_cleanup();
        return 1;
    }

    int optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        thread_pool_destroy(pool);
        files_cleanup();
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        thread_pool_destroy(pool);
        files_cleanup();
        return 1;
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        close(server_fd);
        thread_pool_destroy(pool);
        files_cleanup();
        return 1;
    }

    printf("===========================\n");
    printf("       MicroHTTP Server\n");
    printf("===========================\n");
    printf("Port       : %d\n", port);
    printf("Web Root   : %s\n", FILES_DEFAULT_ROOT);
    printf("Workers    : %d\n", workers);
    printf("Queue      : %d\n", queue_cap);
    printf("Status     : Running\n");
    printf("===========================\n");
    fflush(stdout);

    /*
     * Main thread = producer: accept clients, hand sockets to the
     * pool. It never processes a request itself, so it stays
     * responsive even when clients are slow.
     */
    while (g_shutdown == 0)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);

        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue; /* signal arrived - the while condition rechecks */
            perror("accept");
            continue;
        }

        if (thread_pool_submit(pool, client_fd) < 0)
        {
            /*
             * Queue full or shutdown: close here so every accepted
             * socket is either processed by a worker or closed by us.
             */
            fprintf(stderr, "Connection rejected (queue full or shutting down)\n");
            close(client_fd);
        }
    }

    printf("Shutdown requested - finishing active requests...\n");
    fflush(stdout);

    close(server_fd);          /* stop accepting new connections */
    thread_pool_destroy(pool); /* drain queue, join workers */
    files_cleanup();

    printf("Shutdown complete.\n");
    return 0;
}
