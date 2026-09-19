/* http.h — HTTPS client built on a controlled curl child process.
 *
 * Security properties:
 *  - No shell is used; curl receives an argv array.
 *  - TLS certificate verification stays ON (curl default).
 *  - Redirects are followed manually (max 5) so a forge token is NEVER sent
 *    to a redirect target — only to the exact host it belongs to.
 *  - Only https is allowed by default (http requires explicit allow_http).
 *  - Timeouts, retries, and a size cap are enforced.
 */
#ifndef GF_HTTP_H
#define GF_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_http_opts {
    int timeout_secs;        /* total transfer timeout (default 120) */
    int connect_timeout_secs;
    int retries;             /* transient error retries (default 2) */
    size_t max_bytes;        /* response body cap (default 512 MiB) */
    bool allow_http;         /* permit http:// (self-hosted, opt-in) */
    const char *const *headers; /* extra "Name: value" headers, NULL-term. */
    const char *token;       /* bearer token; only sent when token_host matches */
    const char *token_host;  /* exact host the token may be presented to */
    const char *cafile;      /* extra CA bundle for self-hosted forges
                              * (private PKI); verification stays ON */
} gf_http_opts;

gf_http_opts gf_http_opts_default(void);

/* Parse a URL into scheme/host/port/path (all malloc'd, port may be NULL).
 * Returns 0/-1. */
int gf_url_split(const char *url, char **scheme, char **host, char **port,
                 char **path);

/* GET to memory. *out_body is malloc'd (NUL-terminated for convenience).
 * Returns HTTP status (>=100) on completion, or -1 on client errors,
 * -2 on policy violations (bad scheme/host, too large). */
int gf_http_get(const char *url, const gf_http_opts *opts, char **out_body,
                size_t *out_len);

/* GET to file atomically: downloads to a temp file in the destination
 * directory, optionally verifies sha256, then renames into place.
 * Returns HTTP status, -1 client error, -2 policy violation,
 * -3 checksum mismatch (logged). */
int gf_http_download(const char *url, const char *dest_path,
                     const char *expected_sha256, const gf_http_opts *opts);

/* Is curl usable? (doctor check) Returns 0 and fills version if OK. */
int gf_http_toolcheck(char *version, size_t versionlen);

#endif /* GF_HTTP_H */
