#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>

#define MAX_METHOD_LEN    16
#define MAX_PATH_LEN    2048
#define MAX_VERSION_LEN   16
#define MAX_HEADERS       32
#define MAX_HEADER_NAME   64
#define MAX_HEADER_VALUE  512

typedef struct
{
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
} HttpHeader;

typedef struct
{
    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    char version[MAX_VERSION_LEN];

    HttpHeader headers[MAX_HEADERS];
    int header_count;
} HttpRequest;

typedef enum
{
    PARSE_OK = 0,
    PARSE_ERR_INCOMPLETE = 1,
    PARSE_ERR_MALFORMED = 2
} ParseResult;

ParseResult parser_parse(const char *raw, size_t raw_len, HttpRequest *out);
const char *parser_strerror(ParseResult result);

#endif
