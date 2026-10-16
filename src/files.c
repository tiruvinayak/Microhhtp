#define _DEFAULT_SOURCE

#include "files.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>

/* Opened once in files_init(); read-only afterwards (thread-safe). */
static int web_root_fd = -1;

int files_init(const char *root)
{
    if (root == NULL)
        return -1;

    /* O_DIRECTORY: refuse to use a non-directory as the web root. */
    web_root_fd = open(root, O_RDONLY | O_DIRECTORY);
    return (web_root_fd < 0) ? -1 : 0;
}

void files_cleanup(void)
{
    if (web_root_fd >= 0)
    {
        close(web_root_fd);
        web_root_fd = -1;
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decode "%XX" sequences. Returns 0 on success, -1 on malformed
 * encoding (bad hex, truncated sequence) or if %00 is present.
 * Decoded '/' (%2F) becomes a real separator - checked per component.
 */
static int percent_decode(const char *in, char *out, size_t out_size)
{
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0'; i++)
    {
        char c = in[i];

        if (c == '%')
        {
            /* Check bounds BEFORE reading in[i+2]: a string ending in
             * '%' must not cause a read past the terminator. */
            if (in[i + 1] == '\0' || in[i + 2] == '\0')
                return -1;

            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);

            if (hi < 0 || lo < 0)
                return -1; /* malformed like "%zz" or trailing "%" */

            c = (char)(hi * 16 + lo);
            i += 2;

            if (c == '\0')
                return -1; /* "%00" rejected: would hide data from C strings */
        }

        if (o + 1 >= out_size)
            return -1; /* decoded path would not fit */

        out[o++] = c;
    }

    out[o] = '\0';
    return 0;
}

int files_open_url_path(const char *url_path, int *http_status)
{
    int status_404 = 404;
    if (http_status == NULL)
        http_status = &status_404;

    *http_status = 404;

    if (url_path == NULL || web_root_fd < 0)
    {
        *http_status = 500;
        return -1;
    }

    char decoded[FILES_MAX_PATH];

    if (percent_decode(url_path, decoded, sizeof(decoded)) < 0)
    {
        *http_status = 400; /* malformed percent-encoding */
        return -1;
    }

    if (decoded[0] != '/')
    {
        *http_status = 400;
        return -1;
    }

    /*
     * Walk the path component by component, always relative to an
     * already-open directory fd. Because we never build a full
     * filesystem path string, client input cannot redirect open()
     * outside the web root.
     */
    int current = dup(web_root_fd);
    if (current < 0)
    {
        *http_status = 500;
        return -1;
    }

    const char *p = decoded + 1; /* skip leading '/' */
    int opened = 0;

    for (;;)
    {
        while (*p == '/')
            p++; /* collapse "//" */

        if (*p == '\0')
            break;

        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        int last = (slash == NULL);

        char component[256];
        if (len >= sizeof(component))
        {
            close(current);
            *http_status = 404;
            return -1;
        }

        memcpy(component, p, len);
        component[len] = '\0';

        if (strcmp(component, "..") == 0)
        {
            close(current);
            *http_status = 400; /* traversal attempt */
            return -1;
        }

        if (strcmp(component, ".") == 0)
        {
            p = last ? p + len : slash + 1;
            continue;
        }

        int flags = O_RDONLY | O_NOFOLLOW;
        if (!last)
            flags |= O_DIRECTORY;

        int next_fd = openat(current, component, flags);
        close(current);

        if (next_fd < 0)
        {
            *http_status = 404;
            return -1;
        }

        current = next_fd;
        opened = 1;

        if (last)
            break;

        p = slash + 1;
    }

    /* "/" or "//" etc: no component opened -> default page */
    if (opened == 0)
    {
        int index_fd = openat(current, "index.html", O_RDONLY | O_NOFOLLOW);
        close(current);

        if (index_fd < 0)
        {
            *http_status = 404;
            return -1;
        }

        current = index_fd;
    }

    /* Only regular files are served (no directories, devices, ...). */
    struct stat st;
    if (fstat(current, &st) < 0 || !S_ISREG(st.st_mode))
    {
        close(current);
        *http_status = 404;
        return -1;
    }

    return current;
}

const char *files_content_type(const char *path)
{
    static const struct
    {
        const char *ext;
        const char *type;
    } table[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".js",   "application/javascript; charset=utf-8" },
        { ".json", "application/json; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" }
    };

    if (path == NULL)
        return "application/octet-stream";

    /* Decode first so "%2Ecss" still maps to ".css". */
    char decoded[FILES_MAX_PATH];
    const char *name = path;

    if (percent_decode(path, decoded, sizeof(decoded)) == 0)
        name = decoded;

    const char *dot = strrchr(name, '.');

    /*
     * The default page is always index.html: a bare "/" (or "//" ...)
     * has no extension but is still HTML.
     */
    if (name[0] == '/' && strspn(name, "/") == strlen(name))
        return "text/html; charset=utf-8";

    if (dot == NULL || strchr(dot, '/') != NULL)
        return "application/octet-stream";

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        if (strcasecmp(dot, table[i].ext) == 0)
            return table[i].type;
    }

    return "application/octet-stream";
}
