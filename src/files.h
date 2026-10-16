#ifndef FILES_H
#define FILES_H

#include <stddef.h>

#define FILES_DEFAULT_ROOT "www"
#define FILES_MAX_PATH 2048

/*
 * Open the web root directory once at startup.
 * All later file lookups are relative to this directory fd.
 */
int files_init(const char *root);

/* Close the web root directory fd. */
void files_cleanup(void);

/*
 * Open the file for a URL path such as "/index.html" or "/style.css".
 *
 * Security:
 *   - percent-decodes the path (rejects malformed encodings and %00)
 *   - rejects ".." components (no traversal)
 *   - walks components with openat() relative to the web root fd
 *   - uses O_NOFOLLOW so symlinks cannot escape the web root
 *   - serves only regular files (directories -> 404)
 *
 * Returns a file descriptor (>= 0) on success.
 * On failure returns -1 and sets *http_status to 400 (bad/malicious
 * path) or 404 (not found / not a regular file).
 */
int files_open_url_path(const char *url_path, int *http_status);

/* MIME type from the file extension. Unknown -> application/octet-stream. */
const char *files_content_type(const char *path);

#endif
