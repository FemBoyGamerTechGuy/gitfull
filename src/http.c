/* http.c — HTTPS via controlled curl child processes. */
#include "http.h"

#include "common.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GF_HTTP_MAX_REDIRECTS 5
#define GF_USER_AGENT "gitfull/0.1.0 (+https://github.com/FemBoyGamerTechGuy/gitfull)"

gf_http_opts gf_http_opts_default(void)
{
    gf_http_opts o = { 0 };
    o.timeout_secs = 120;
    o.connect_timeout_secs = 15;
    o.retries = 2;
    o.max_bytes = 512u * 1024u * 1024u;
    o.allow_http = false;
    o.headers = NULL;
    o.token = NULL;
    o.token_host = NULL;
    o.cafile = NULL;
    return o;
}

static int valid_scheme(const char *url, bool allow_http)
{
    if (gf_str_starts_with(url, "https://"))
        return 0;
    if (allow_http && gf_str_starts_with(url, "http://"))
        return 0;
    gf_log(GF_LOG_ERROR, "http: refusing non-https URL: %s", url);
    return -1;
}

int gf_url_split(const char *url, char **scheme, char **host, char **port,
                 char **path)
{
    *scheme = *host = *port = *path = NULL;
    if (!url)
        return -1;
    const char *sp = strstr(url, "://");
    if (!sp)
        return -1;
    *scheme = gf_strndup(url, (size_t)(sp - url));
    const char *p = sp + 3;
    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostlen == 0)
        return -1;
    char *authority = gf_strndup(p, hostlen);
    char *colon = strrchr(authority, ':');
    if (colon) {
        *colon = '\0';
        *port = gf_strdup(colon + 1);
        char *br = strchr(authority, ']'); /* strip [..] around v6 literals */
        if (authority[0] == '[' && br) {
            memmove(authority, authority + 1, (size_t)(br - authority));
            *br = '\0';
            *br = '\0';
        }
    }
    *host = gf_strdup(authority);
    free(authority);
    /* lowercase the host */
    for (char *h = *host; *h; h++)
        *h = (char)tolower((unsigned char)*h);
    *path = gf_strdup(slash ? slash : "/");
    if ((*host)[0] == '\0') {
        return -1;
    }
    return 0;
}

/* One curl invocation (no auto-redirect). Writes body to out_fd via -o,
 * report line "%{http_code} %{redirect_url}" captured from stdout.
 * Returns HTTP status code, or -1 on curl invocation failure. */
static int curl_once(const char *url, const gf_http_opts *opts,
                     const char *out_file, char **redirect_out)
{
    *redirect_out = NULL;
    gf_strbuf argv_sb;
    gf_strbuf_init(&argv_sb);

    char tmo[32], cto[32], retry[16], maxsz[32];
    snprintf(tmo, sizeof(tmo), "%d",
             opts->timeout_secs > 0 ? opts->timeout_secs : 120);
    snprintf(cto, sizeof(cto), "%d",
             opts->connect_timeout_secs > 0 ? opts->connect_timeout_secs : 15);
    snprintf(retry, sizeof(retry), "%d", opts->retries > 0 ? opts->retries : 0);
    snprintf(maxsz, sizeof(maxsz), "%zu",
             opts->max_bytes > 0 ? opts->max_bytes : (size_t)512 * 1024 * 1024);

    /* token header only when the URL host matches token_host exactly */
    bool send_token = false;
    if (opts->token && opts->token_host && *opts->token_host) {
        char *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
        if (gf_url_split(url, &scheme, &host, &port, &path) == 0) {
            send_token = gf_str_ieq(host, opts->token_host);
        }
        free(scheme); free(host); free(port); free(path);
    }

    char auth_hdr[512];
    auth_hdr[0] = '\0';
    if (send_token)
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s",
                 opts->token);

#define CURL_ARGV_MAX 64
    const char *argv[CURL_ARGV_MAX];
    size_t ai = 0;
    /* helper pushing argv entries with an overflow guard */
#define PUSH_ARG(v)                                                          \
    do {                                                                     \
        if (ai + 1 >= CURL_ARGV_MAX) {                                       \
            gf_log(GF_LOG_ERROR, "http: internal argv overflow");            \
            gf_strbuf_free(&argv_sb);                                        \
            return -1;                                                       \
        }                                                                    \
        argv[ai++] = (v);                                                    \
    } while (0)
    PUSH_ARG("curl");
    PUSH_ARG("--silent");
    PUSH_ARG("--show-error");
    PUSH_ARG("--fail-with-body");
    PUSH_ARG("--no-progress-meter");
    PUSH_ARG("--location-trusted"); /* placeholder replaced below */
    /* NOTE: we do NOT auto-follow redirects; redirect policy is ours. */
    argv[ai - 1] = "--max-redirs";
    PUSH_ARG("0");
    PUSH_ARG("--proto");
    argv[ai++] = opts->allow_http ? "-all,http,https" : "-all,https";
    PUSH_ARG("--proto-redir");
    argv[ai++] = opts->allow_http ? "-all,http,https" : "-all,https";
    PUSH_ARG("--connect-timeout");
    PUSH_ARG(cto);
    PUSH_ARG("--max-time");
    PUSH_ARG(tmo);
    if (opts->retries > 0) {
        PUSH_ARG("--retry");
        PUSH_ARG(retry);
        PUSH_ARG("--retry-all-errors");
        PUSH_ARG("--retry-delay");
        PUSH_ARG("2");
    }
    PUSH_ARG("--max-filesize");
    PUSH_ARG(maxsz);
    PUSH_ARG("--user-agent");
    PUSH_ARG(GF_USER_AGENT);
    PUSH_ARG("--output");
    PUSH_ARG(out_file);
    PUSH_ARG("--write-out");
    PUSH_ARG("%{http_code} %{redirect_url}");
    if (opts->cafile && *opts->cafile) {
        /* private PKI: add the CA while keeping system verification on */
        PUSH_ARG("--cacert");
        PUSH_ARG(opts->cafile);
    }
    if (send_token) {
        PUSH_ARG("--header");
        PUSH_ARG(auth_hdr);
    }
    for (size_t i = 0; opts->headers && opts->headers[i]; i++) {
        if (ai + 2 < 32) {
            PUSH_ARG("--header");
            argv[ai++] = opts->headers[i];
        }
    }
    PUSH_ARG("--");
    PUSH_ARG(url);
    argv[ai] = NULL;

    gf_strbuf report;
    gf_strbuf_init(&report);
    gf_exec_opts eo = gf_exec_opts_default();
    eo.out = &report;
    eo.timeout_ms = (uint64_t)(opts->timeout_secs + 30) * 1000u;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    int rc = gf_exec_capture(argv, &eo, &res);
    if (rc != 0) {
        gf_strbuf_free(&report);
        gf_strbuf_free(&argv_sb);
        return -1;
    }
    gf_strbuf_free(&argv_sb);
    if (res.signaled || res.timed_out) {
        gf_log(GF_LOG_ERROR, "http: curl killed (%s)",
               res.timed_out ? "timeout" : "signal");
        gf_strbuf_free(&report);
        return -1;
    }

    /* parse "CODE REDIRECT_URL" */
    char *rep = gf_strbuf_steal(&report);
    gf_str_trim(rep);
    char *sp = strchr(rep, ' ');
    int code = -1;
    if (sp) {
        *sp = '\0';
        *redirect_out = gf_strdup(sp + 1);
        gf_str_trim(*redirect_out);
    }
    errno = 0;
    long c = strtol(rep, NULL, 10);
    if (errno == 0 && rep[0] != '\0')
        code = (int)c;
    free(rep);

    if (code < 100) {
        /* curl failed to even complete the request: could be --fail-with-body
         * with no response. */
        gf_log(GF_LOG_DEBUG, "http: curl gave no usable status for %s", url);
    }
    return code;
}

static int follow_get(const char *url_in, const gf_http_opts *opts_in,
                      const char *out_file, char **final_url)
{
    gf_http_opts opts = *opts_in;
    char *url = gf_strdup(url_in);
    char *redirect = NULL;
    int code = -1;
    for (int hop = 0; hop <= GF_HTTP_MAX_REDIRECTS; hop++) {
        free(redirect);
        redirect = NULL;
        code = curl_once(url, &opts, out_file, &redirect);
        if (code < 100) {
            free(url);
            free(redirect);
            return -1;
        }
        if (code >= 300 && code < 400) {
            if (!redirect || !*redirect) {
                gf_log(GF_LOG_ERROR, "http: %ld redirect without Location", (long)code);
                free(url);
                return -1;
            }
            if (hop == GF_HTTP_MAX_REDIRECTS) {
                gf_log(GF_LOG_ERROR, "http: too many redirects (> %d)",
                       GF_HTTP_MAX_REDIRECTS);
                free(url);
                return -1;
            }
            if (valid_scheme(redirect, opts.allow_http) != 0) {
                free(url);
                free(redirect);
                return -2;
            }
            free(url);
            url = gf_strdup(redirect);
            continue;
        }
        break;
    }
    free(redirect);
    if (final_url)
        *final_url = url;
    else
        free(url);
    return code;
}

int gf_http_get(const char *url, const gf_http_opts *opts, char **out_body,
                size_t *out_len)
{
    if (out_body)
        *out_body = NULL;
    if (out_len)
        *out_len = 0;
    if (valid_scheme(url, opts ? opts->allow_http : false) != 0)
        return -2;

    gf_http_opts o = opts ? *opts : gf_http_opts_default();
    if (!o.max_bytes)
        o.max_bytes = 512u * 1024u * 1024u;

    char *dir = gf_path_dirname("/tmp/.gitfull-http");
    /* body temp file in /tmp-ish: use TMPDIR then */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";
    char *out_file = NULL;
    int fd = gf_mkstemp_in(tmpdir, &out_file);
    free(dir);
    if (fd < 0)
        return -1;
    close(fd);

    char *final_url = NULL;
    int code = -1;
    struct stat rst = { 0 };
    /* Transient-failure retries: empty-body 403 / 429 / 5xx. Egress
     * proxies sometimes emit empty 403 hiccups; genuine API rate limits
     * carry a JSON body and are surfaced to the caller instead. */
    for (int attempt = 0; attempt < 4; attempt++) {
        code = follow_get(url, &o, out_file, &final_url);
        if (code < 100)
            break;
        bool empty_403 = code == 403 &&
                         (stat(out_file, &rst) != 0 || rst.st_size == 0);
        if ((code == 429 || code >= 500 || empty_403) && attempt < 3) {
            int delays[4] = { 1, 2, 4, 8 };
            gf_log(GF_LOG_DEBUG, "http: transient %d, retrying in %ds", code,
                   delays[attempt]);
            struct timespec ts = { delays[attempt], 0 };
            nanosleep(&ts, NULL);
            continue;
        }
        break;
    }
    if (code < 100) {
        unlink(out_file);
        free(out_file);
        free(final_url);
        return -1;
    }
    if (code == 429 || code >= 500) {
        unlink(out_file);
        free(out_file);
        free(final_url);
        return code;
    }

    /* enforce body size cap */
    struct stat st;
    size_t body_len = 0;
    char *body = NULL;
    if (stat(out_file, &st) == 0 && st.st_size >= 0) {
        body_len = (size_t)st.st_size;
        if (body_len > o.max_bytes) {
            gf_log(GF_LOG_ERROR, "http: response too large: %zu bytes (cap %zu)",
                   body_len, o.max_bytes);
            unlink(out_file);
            free(out_file);
            free(final_url);
            return -2;
        }
        body = gf_fs_read_file_limit(out_file, o.max_bytes + 1, &body_len);
    }
    unlink(out_file);
    free(out_file);
    free(final_url);
    if (code >= 400) {
        free(body);
        /* include error body excerpt when useful */
        return code;
    }
    if (!body) {
        gf_log(GF_LOG_ERROR, "http: failed to read response body");
        return -1;
    }
    if (out_body) {
        *out_body = body;
        if (out_len)
            *out_len = body_len;
    } else {
        free(body);
    }
    return code;
}

int gf_http_download(const char *url, const char *dest_path,
                     const char *expected_sha256, const gf_http_opts *opts)
{
    if (valid_scheme(url, opts ? opts->allow_http : false) != 0)
        return -2;
    gf_http_opts o = opts ? *opts : gf_http_opts_default();
    if (!o.max_bytes)
        o.max_bytes = 2u * 1024u * 1024u * 1024u;

    char *dir = gf_path_dirname(dest_path);
    char *tmp = NULL;
    int fd = gf_mkstemp_in(dir, &tmp);
    if (fd < 0) {
        free(dir);
        return -1;
    }
    close(fd);
    unlink(tmp); /* curl will create it */

    char *final_url = NULL;
    int code = follow_get(url, &o, tmp, &final_url);
    free(final_url);
    if (code < 100) {
        gf_log(GF_LOG_ERROR, "http: download failed: %s", url);
        unlink(tmp);
        free(tmp);
        free(dir);
        return -1;
    }
    if (code == 429 || (code >= 500 && code < 600)) {
        unlink(tmp);
        free(tmp);
        free(dir);
        return code;
    }
    if (code >= 400) {
        gf_log(GF_LOG_ERROR, "http: download of %s failed with status %d", url,
               code);
        unlink(tmp);
        free(tmp);
        free(dir);
        return code;
    }

    /* enforce size cap */
    struct stat st;
    if (stat(tmp, &st) != 0 || st.st_size < 0 ||
        (size_t)st.st_size > o.max_bytes) {
        gf_log(GF_LOG_ERROR, "http: downloaded file exceeds size cap");
        unlink(tmp);
        free(tmp);
        free(dir);
        return -2;
    }

    if (expected_sha256 && *expected_sha256) {
        char *actual = gf_sha256_file_hex(tmp);
        if (!actual) {
            unlink(tmp);
            free(tmp);
            free(dir);
            return -1;
        }
        if (gf_ct_memcmp(actual, expected_sha256, strlen(expected_sha256)) != 0 ||
            strlen(actual) != strlen(expected_sha256)) {
            gf_log(GF_LOG_ERROR,
                   "checksum mismatch for %s:\n  expected %s\n  actual   %s",
                   url, expected_sha256, actual);
            free(actual);
            unlink(tmp);
            free(tmp);
            free(dir);
            return -3;
        }
        free(actual);
    }

    if (rename(tmp, dest_path) != 0) {
        gf_log_errno(GF_LOG_ERROR, "rename");
        unlink(tmp);
        free(tmp);
        free(dir);
        return -1;
    }
    gf_fs_fsync_dir(dir);
    free(tmp);
    free(dir);
    return code;
}

int gf_http_toolcheck(char *version, size_t versionlen)
{
    const char *argv[] = { "curl", "--version", NULL };
    gf_strbuf out;
    gf_strbuf_init(&out);
    gf_exec_opts eo = gf_exec_opts_default();
    eo.out = &out;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    if (gf_exec_capture(argv, &eo, &res) != 0 || res.status != 0) {
        gf_strbuf_free(&out);
        return -1;
    }
    /* first line: curl 8.14.1 (...) ... */
    char *line = gf_strbuf_steal(&out);
    char *nl = strchr(line, '\n');
    if (nl)
        *nl = '\0';
    if (gf_str_starts_with(line, "curl ")) {
        if (version)
            snprintf(version, versionlen, "%s", line + 5);
        free(line);
        return 0;
    }
    free(line);
    return -1;
}
