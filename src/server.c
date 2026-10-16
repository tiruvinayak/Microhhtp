#define _DEFAULT_SOURCE

#include "server.h"
#include "parser.h"
#include "files.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>

/* One mutex so log lines from different workers do not interleave. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void server_log(int status, const char *method, const char *path)
{
    char line[600];
    int n;

    if (status > 0)
        n = snprintf(line, sizeof(line), "[%d] %s %s\n",
                     status, method ? method : "-", path ? path : "-");
    else
        n = snprintf(line, sizeof(line), "[--] %s %s\n",
                     method ? method : "-", path ? path : "-");

    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;

    pthread_mutex_lock(&log_mutex);
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

/*
 * write() on a socket may send fewer bytes than requested (partial
 * write). Loop until every byte is written, retry on EINTR.
 */
static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;

    while (len > 0)
    {
        ssize_t n = write(fd, p, len);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

/* Bound how long a worker can be stuck on one client. */
static void set_client_timeouts(int fd)
{
    struct timeval tv;
    tv.tv_sec = SERVER_CLIENT_TIMEOUT_SEC;
    tv.tv_usec = 0;

    /* Ignore errors: timeouts are best-effort. */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/*
 * Read until the header section is complete ("\r\n\r\n"), the buffer
 * is full, the client disconnects, or the read times out.
 *
 * Returns: >0  number of bytes read (headers complete)
 *          -1  read error / timeout / early disconnect
 *          -2  request larger than SERVER_MAX_REQUEST (never terminated)
 */
static int read_request(int fd, char *buf, size_t buf_size)
{
    size_t used = 0;

    while (used < buf_size - 1)
    {
        ssize_t n = read(fd, buf + used, buf_size - 1 - used);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1; /* timeout (EAGAIN) or real error */
        }

        if (n == 0)
            break; /* client closed */

        used += (size_t)n;

        /* Scan raw bytes (not strstr: request may contain NUL bytes). */
        for (size_t i = 0; used >= 4 && i + 3 < used; i++)
        {
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n')
            {
                buf[used] = '\0';
                return (int)used;
            }
        }
    }

    buf[used] = '\0';

    if (used >= buf_size - 1)
        return -2; /* filled the buffer without a header terminator */

    return (used > 0) ? (int)used : -1;
}

/*
 * Send a small text response (used for errors and 405).
 * extra_header: e.g. "Allow: GET, HEAD\r\n", or NULL.
 * send_body: 0 for HEAD requests (headers only).
 */
static void send_simple_response(int fd, const char *version,
                                 int status, const char *reason,
                                 const char *content_type,
                                 const char *extra_header,
                                 const char *body,
                                 int send_body)
{
    char header[1024];

    int n = snprintf(header, sizeof(header),
                     "%s %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     version, status, reason,
                     content_type, strlen(body),
                     extra_header ? extra_header : "");

    if (n > 0 && (size_t)n < sizeof(header))
    {
        if (write_all(fd, header, (size_t)n) < 0)
            return;
    }

    if (send_body && body[0] != '\0')
        (void)write_all(fd, body, strlen(body));
}

/*
 * Send "200 OK" headers plus the file contents.
 * send_body: 0 for HEAD (same headers, no body).
 * The file is streamed in small chunks - never loaded fully into memory.
 */
static int send_file_response(int fd, const char *version, int file_fd,
                              const char *content_type, int send_body)
{
    struct stat st;

    if (fstat(file_fd, &st) < 0)
        return -1;

    char header[1024];

    int n = snprintf(header, sizeof(header),
                     "%s 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     version, content_type, (long)st.st_size);

    if (n <= 0 || (size_t)n >= sizeof(header))
        return -1;

    if (write_all(fd, header, (size_t)n) < 0)
        return -1;

    if (!send_body)
        return 0;

    char buf[4096];
    ssize_t r;

    while ((r = read(file_fd, buf, sizeof(buf))) > 0)
    {
        if (write_all(fd, buf, (size_t)r) < 0)
            return -1;
    }

    return (r < 0) ? -1 : 0;
}

void server_handle_client(int client_fd)
{
    set_client_timeouts(client_fd);

    char buf[SERVER_MAX_REQUEST];
    int r = read_request(client_fd, buf, sizeof(buf));

    if (r == -2)
    {
        server_log(400, "-", "(request too large)");
        send_simple_response(client_fd, "HTTP/1.1",
                             400, "Bad Request",
                             "text/plain; charset=utf-8",
                             NULL, "Bad Request", 1);
        return;
    }

    if (r < 0)
    {
        /* Timeout or disconnect before a complete request: close only. */
        server_log(0, "-", "(no complete request)");
        return;
    }

    HttpRequest req;

    if (parser_parse(buf, (size_t)r, &req) != PARSE_OK)
    {
        server_log(400, "-", "-");
        send_simple_response(client_fd, "HTTP/1.1",
                             400, "Bad Request",
                             "text/plain; charset=utf-8",
                             NULL, "Bad Request", 1);
        return;
    }

    /* Only GET and HEAD are supported. */
    if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0)
    {
        server_log(405, req.method, req.path);
        send_simple_response(client_fd, req.version,
                             405, "Method Not Allowed",
                             "text/plain; charset=utf-8",
                             "Allow: GET, HEAD\r\n",
                             "Method Not Allowed\n", 1);
        return;
    }

    int send_body = (strcmp(req.method, "HEAD") != 0);

    int status = 404;
    int file_fd = files_open_url_path(req.path, &status);

    if (file_fd < 0)
    {
        if (status == 400)
        {
            server_log(400, req.method, req.path);
            send_simple_response(client_fd, req.version,
                                 400, "Bad Request",
                                 "text/plain; charset=utf-8",
                                 NULL, "Bad Request", send_body);
        }
        else
        {
            server_log(404, req.method, req.path);
            send_simple_response(client_fd, req.version,
                                 404, "Not Found",
                                 "text/plain; charset=utf-8",
                                 NULL, "File Not Found\n", send_body);
        }
        return;
    }

    const char *content_type = files_content_type(req.path);

    if (send_file_response(client_fd, req.version, file_fd,
                           content_type, send_body) < 0)
    {
        server_log(0, req.method, req.path); /* client vanished mid-send */
    }
    else
    {
        server_log(200, req.method, req.path);
    }

    close(file_fd);
}
