#define _DEFAULT_SOURCE

#include "parser.h"

#include <string.h>

/*
 * Find the first CRLF ("\r\n") at index >= 'from'.
 * Returns 1 and stores the index of '\r' in *idx, or 0 if not found.
 */
static int find_crlf(const char *s, size_t len, size_t from, size_t *idx)
{
    for (size_t i = from; i + 1 < len; i++)
    {
        if (s[i] == '\r' && s[i + 1] == '\n')
        {
            *idx = i;
            return 1;
        }
    }

    return 0;
}

/*
 * Find the first CRLFCRLF ("\r\n\r\n") that ends the header section.
 * Returns 1 and stores the index in *idx, or 0 if not found.
 */
static int find_header_end(const char *s, size_t len, size_t *idx)
{
    for (size_t i = 0; i + 3 < len; i++)
    {
        if (s[i] == '\r' && s[i + 1] == '\n' &&
            s[i + 2] == '\r' && s[i + 3] == '\n')
        {
            *idx = i;
            return 1;
        }
    }

    return 0;
}

ParseResult parser_parse(const char *raw, size_t raw_len, HttpRequest *out)
{
    if (raw == NULL || out == NULL)
        return PARSE_ERR_MALFORMED;

    memset(out, 0, sizeof(*out));

    /*
     * The header section must be complete before anything is parsed.
     * A TCP read can return a partial request; that is not bad data.
     * Phase 6 will buffer until the request is complete.
     */
    size_t header_end;
    if (!find_header_end(raw, raw_len, &header_end))
        return PARSE_ERR_INCOMPLETE;

    /* ---- request line: METHOD SP PATH SP VERSION ---- */
    size_t line_end;
    if (!find_crlf(raw, raw_len, 0, &line_end) || line_end > header_end)
        return PARSE_ERR_MALFORMED;

    char line[MAX_METHOD_LEN + MAX_PATH_LEN + MAX_VERSION_LEN + 8];

    if (line_end >= sizeof(line))
        return PARSE_ERR_MALFORMED;

    memcpy(line, raw, line_end);
    line[line_end] = '\0';

    /* METHOD: 1..MAX_METHOD_LEN-1 uppercase letters */
    size_t p = 0;
    while (p < line_end && line[p] != ' ')
        p++;

    if (p == 0 || p == line_end)
        return PARSE_ERR_MALFORMED;

    size_t method_len = p;
    if (method_len >= MAX_METHOD_LEN)
        return PARSE_ERR_MALFORMED;

    for (size_t i = 0; i < method_len; i++)
    {
        if (line[i] < 'A' || line[i] > 'Z')
            return PARSE_ERR_MALFORMED;
    }

    memcpy(out->method, line, method_len);
    out->method[method_len] = '\0';

    /* exactly one space, then PATH */
    p++;
    size_t path_start = p;

    while (p < line_end && line[p] != ' ')
        p++;

    if (p == path_start || p == line_end)
        return PARSE_ERR_MALFORMED;

    size_t path_len = p - path_start;
    if (path_len >= MAX_PATH_LEN)
        return PARSE_ERR_MALFORMED;
    if (line[path_start] != '/')
        return PARSE_ERR_MALFORMED;

    for (size_t i = path_start; i < p; i++)
    {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c == 0x7f)
            return PARSE_ERR_MALFORMED;
    }

    memcpy(out->path, line + path_start, path_len);
    out->path[path_len] = '\0';

    /* exactly one space, then VERSION (rest of line, must contain no space) */
    p++;
    if (p == line_end)
        return PARSE_ERR_MALFORMED;

    size_t version_len = line_end - p;
    if (version_len >= MAX_VERSION_LEN)
        return PARSE_ERR_MALFORMED;

    for (size_t i = p; i < line_end; i++)
    {
        if (line[i] == ' ' || line[i] == '\t')
            return PARSE_ERR_MALFORMED;
    }

    if (!((version_len == 8 && memcmp(line + p, "HTTP/1.1", 8) == 0) ||
          (version_len == 8 && memcmp(line + p, "HTTP/1.0", 8) == 0)))
    {
        return PARSE_ERR_MALFORMED;
    }

    memcpy(out->version, line + p, version_len);
    out->version[version_len] = '\0';

    /* ---- header fields: Name ":" OWS value OWS ---- */
    size_t pos = line_end + 2;

    while (pos < header_end)
    {
        size_t eol;
        if (!find_crlf(raw, raw_len, pos, &eol) || eol > header_end)
            return PARSE_ERR_MALFORMED;

        const char *field = raw + pos;
        size_t field_len = eol - pos;

        /* name must be non-empty and terminated by a colon */
        size_t colon = 0;
        while (colon < field_len && field[colon] != ':')
            colon++;

        if (colon == 0 || colon == field_len)
            return PARSE_ERR_MALFORMED;

        if (colon >= MAX_HEADER_NAME)
            return PARSE_ERR_MALFORMED;

        /* name: printable ASCII, no spaces (0x21..0x7e) */
        for (size_t i = 0; i < colon; i++)
        {
            unsigned char c = (unsigned char)field[i];
            if (c < 0x21 || c > 0x7e)
                return PARSE_ERR_MALFORMED;
        }

        /* value: trim optional whitespace after ':' and at end of line */
        size_t v_start = colon + 1;
        while (v_start < field_len &&
               (field[v_start] == ' ' || field[v_start] == '\t'))
        {
            v_start++;
        }

        size_t v_end = field_len;
        while (v_end > v_start &&
               (field[v_end - 1] == ' ' || field[v_end - 1] == '\t'))
        {
            v_end--;
        }

        size_t value_len = v_end - v_start;
        if (value_len >= MAX_HEADER_VALUE)
            return PARSE_ERR_MALFORMED;

        /* value: no control characters except tab */
        for (size_t i = v_start; i < v_end; i++)
        {
            unsigned char c = (unsigned char)field[i];
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return PARSE_ERR_MALFORMED;
        }

        if (out->header_count >= MAX_HEADERS)
            return PARSE_ERR_MALFORMED;

        HttpHeader *hdr = &out->headers[out->header_count];

        memcpy(hdr->name, field, colon);
        hdr->name[colon] = '\0';

        if (value_len > 0)
            memcpy(hdr->value, field + v_start, value_len);
        hdr->value[value_len] = '\0';

        out->header_count++;

        pos = eol + 2;
    }

    return PARSE_OK;
}

const char *parser_strerror(ParseResult result)
{
    switch (result)
    {
        case PARSE_OK:
            return "ok";
        case PARSE_ERR_INCOMPLETE:
            return "incomplete request";
        case PARSE_ERR_MALFORMED:
            return "malformed request";
        default:
            return "unknown parse error";
    }
}
