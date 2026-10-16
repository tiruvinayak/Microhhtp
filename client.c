#define _DEFAULT_SOURCE

/*
 * MicroHTTP interactive test client.
 *
 * Connects to 127.0.0.1:8080 (or [port] given on the command line),
 * reads a line of space-separated commands with fgets(), and performs
 * one HTTP request per command, printing each response between
 * separator lines.
 *
 * Commands:
 *   home | about | contact | services | team | help | head |
 *   missing | post | security | exit
 *
 * Any other word is served as a page name: "projects" -> GET
 * /projects.html (no client change needed for new pages). A word that
 * already ends in ".html", or starts with "/", is used as given.
 *
 * One connection per request: the server replies with
 * "Connection: close" (documented protocol behavior), so the client
 * reads each response until the connection closes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_HOST      "127.0.0.1"
#define DEFAULT_PORT      8080
#define MAX_LINE          1024
#define MAX_PATH          512
#define MAX_STATUS_LINE   128
#define CLIENT_TIMEOUT_SEC 10

#define SEP "=================================================="

static const struct
{
    const char *name;
    const char *method;
    const char *path;
} COMMANDS[] = {
    { "home",     "GET",  "/" },
    { "about",    "GET",  "/about.html" },
    { "contact",  "GET",  "/contact.html" },
    { "services", "GET",  "/services.html" },
    { "team",     "GET",  "/team.html" },
    { "help",     "GET",  "/help.html" },
    { "head",     "HEAD", "/" },
    { "missing",  "GET",  "/missing.html" },
    { "post",     "POST", "/" }
};

/* Same partial-write-safe loop as the server. */
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

static int connect_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return -1;
    }

    /* Never hang forever on a broken server. */
    struct timeval tv;
    tv.tv_sec = CLIENT_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, DEFAULT_HOST, &addr.sin_addr) != 1)
    {
        fprintf(stderr, "invalid address %s\n", DEFAULT_HOST);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "cannot connect to %s:%d - %s\n",
                DEFAULT_HOST, port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int send_request(int fd, const char *method, const char *path)
{
    char req[1024];

    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: microhttp-client/1.0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     method, path, DEFAULT_HOST);

    if (n <= 0 || (size_t)n >= sizeof(req))
    {
        fprintf(stderr, "request too long\n");
        return -1;
    }

    return write_all(fd, req, (size_t)n);
}

/* Read until the server closes (Connection: close) and echo everything:
 * status line, headers, and body, exactly as received. */
static void stream_response(int fd)
{
    char buf[4096];
    size_t total = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        fwrite(buf, 1, (size_t)n, stdout);
        total += (size_t)n;
    }

    if (n < 0)
        fprintf(stderr, "\nread error: %s\n", strerror(errno));
    else if (total == 0)
        printf("(server closed the connection without a response)\n");
}

/* Read only the status line of a response; returns the HTTP status
 * code, or -1 on error. The body is deliberately NOT read/printed. */
static int read_status_code(int fd)
{
    char buf[MAX_STATUS_LINE];
    size_t used = 0;
    ssize_t n;

    while (used < sizeof(buf) - 1)
    {
        n = read(fd, buf + used, sizeof(buf) - 1 - used);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;

        used += (size_t)n;

        if (memchr(buf, '\n', used) != NULL)
            break; /* first line complete */
    }

    buf[used] = '\0';

    int major = 0, minor = 0, code = 0;
    if (sscanf(buf, "HTTP/%d.%d %d", &major, &minor, &code) == 3)
        return code;

    return -1;
}

/*
 * Controlled traversal-security test: sends two known-bad paths and
 * prints only the verdict (status code) - response bodies are never
 * displayed, so no filesystem contents can leak.
 */
static void run_security(int port)
{
    static const char *const tests[] = {
        "/../../../../etc/passwd",
        "/%2e%2e/%2e%2e/etc/passwd"
    };
    const size_t n_tests = sizeof(tests) / sizeof(tests[0]);
    int all_rejected = 1;

    for (size_t i = 0; i < n_tests; i++)
    {
        printf("traversal %-28s -> ", tests[i]);

        int fd = connect_server(port);
        if (fd < 0)
        {
            printf("SKIP (server unreachable)\n");
            all_rejected = 0;
            continue;
        }

        if (send_request(fd, "GET", tests[i]) < 0)
        {
            printf("SKIP (send failed)\n");
            all_rejected = 0;
            close(fd);
            continue;
        }

        int code = read_status_code(fd);
        close(fd);

        if (code >= 400 && code < 500)
            printf("REJECTED (HTTP %d)\n", code);
        else if (code > 0)
        {
            printf("NOT REJECTED (HTTP %d) - SECURITY FAILURE\n", code);
            all_rejected = 0;
        }
        else
        {
            printf("NO VALID RESPONSE\n");
            all_rejected = 0;
        }
    }

    printf("security result: %s\n", all_rejected ? "PASS" : "FAIL");
}

/* Map a page name to "/name.html" (or pass through paths). */
static int build_path(const char *cmd, char *out, size_t out_size)
{
    int n;
    size_t len = strlen(cmd);

    if (cmd[0] == '/')
        n = snprintf(out, out_size, "%s", cmd);
    else if (len > 5 && strcmp(cmd + len - 5, ".html") == 0)
        n = snprintf(out, out_size, "/%s", cmd);
    else
        n = snprintf(out, out_size, "/%s.html", cmd);

    if (n <= 0 || (size_t)n >= out_size)
        return -1;
    return 0;
}

static int command_is_valid(const char *cmd)
{
    if (*cmd == '\0')
        return 0;

    for (const char *p = cmd; *p != '\0'; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f)
            return 0;
    }

    return 1;
}

static void process_command(int port, const char *cmd)
{
    const char *method = "GET";
    const char *path = NULL;
    char pathbuf[MAX_PATH];

    printf("%s\n>> %s\n%s\n", SEP, cmd, SEP);

    if (strcmp(cmd, "security") == 0)
    {
        run_security(port);
        printf("%s\n", SEP);
        return;
    }

    if (!command_is_valid(cmd))
    {
        printf("invalid command (control characters or empty)\n\n%s\n", SEP);
        return;
    }

    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++)
    {
        if (strcmp(cmd, COMMANDS[i].name) == 0)
        {
            method = COMMANDS[i].method;
            path = COMMANDS[i].path;
            break;
        }
    }

    if (path == NULL)
    {
        if (build_path(cmd, pathbuf, sizeof(pathbuf)) < 0)
        {
            printf("command too long: '%.64s'\n\n%s\n", cmd, SEP);
            return;
        }
        path = pathbuf;
    }

    int fd = connect_server(port);
    if (fd < 0)
    {
        /* error already printed to stderr */
        printf("%s\n", SEP);
        return;
    }

    if (send_request(fd, method, path) < 0)
    {
        fprintf(stderr, "send failed: %s\n", strerror(errno));
        close(fd);
        printf("%s\n", SEP);
        return;
    }

    printf("%s %s\n", method, path);
    stream_response(fd);
    close(fd);

    printf("\n%s\n", SEP);
}

static int parse_port(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || *s == '\0')
        return -1;

    errno = 0;
    v = strtol(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < 1 || v > 65535)
        return -1;

    *out = (int)v;
    return 0;
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;

    if (argc == 2)
    {
        if (strcmp(argv[1], "-h") == 0)
        {
            printf("Usage: %s [port]\n", argv[0]);
            return 0;
        }
        if (parse_port(argv[1], &port) < 0)
        {
            fprintf(stderr, "Invalid port: '%s' (expected 1-65535)\n", argv[1]);
            return 1;
        }
    }
    else if (argc > 2)
    {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }

    /* A server that closes mid-response must not kill the client. */
    signal(SIGPIPE, SIG_IGN);

    printf("MicroHTTP client -> %s:%d\n", DEFAULT_HOST, port);
    printf("commands: home about contact services team help\n"
           "          head missing post security exit\n"
           "          (any other word is served as /<word>.html)\n");

    char line[MAX_LINE];

    for (;;)
    {
        printf("MicroHTTP> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL)
        {
            printf("\n");
            break; /* EOF or read error */
        }

        if (strchr(line, '\n') == NULL && !feof(stdin))
        {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF)
                ; /* drain the rest of the over-long line */
            fprintf(stderr, "input line too long (max %d) - ignored\n",
                    MAX_LINE - 1);
            continue;
        }

        char *save = NULL;
        char *tok = strtok_r(line, " \t\r\n", &save);

        while (tok != NULL)
        {
            if (strcmp(tok, "exit") == 0)
                return 0;

            process_command(port, tok);
            tok = strtok_r(NULL, " \t\r\n", &save);
        }
    }

    return 0;
}
