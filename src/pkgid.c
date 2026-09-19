/* pkgid.c — package identifier parsing. */
#include "pkgid.h"

#include "common.h"

#include <stdlib.h>
#include <string.h>

void gf_pkgref_free(gf_pkgref *r)
{
    if (!r)
        return;
    free(r->raw);
    free(r->forge);
    free(r->host);
    free(r->owner);
    free(r->repo);
    free(r->url);
    free(r->local_path);
    free(r->version);
    memset(r, 0, sizeof(*r));
}

gf_pkgref gf_pkgref_copy(const gf_pkgref *r)
{
    gf_pkgref out = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, false };
    out.raw = gf_strdup(r->raw ? r->raw : "");
    out.forge = r->forge ? gf_strdup(r->forge) : NULL;
    out.host = r->host ? gf_strdup(r->host) : NULL;
    out.owner = r->owner ? gf_strdup(r->owner) : NULL;
    out.repo = r->repo ? gf_strdup(r->repo) : NULL;
    out.url = r->url ? gf_strdup(r->url) : NULL;
    out.local_path = r->local_path ? gf_strdup(r->local_path) : NULL;
    out.version = r->version ? gf_strdup(r->version) : NULL;
    out.local = r->local;
    return out;
}

/* strip trailing "/.git" or ".git" suffix in place */
static void strip_git_suffix(char *s)
{
    if (!s)
        return;
    size_t l = strlen(s);
    if (l > 5 && strcmp(s + l - 5, "/.git") == 0) {
        s[l - 5] = '\0';
        return;
    }
    if (l > 4 && strcmp(s + l - 4, ".git") == 0)
        s[l - 4] = '\0';
}

/* split "owner/rest..." into owner + rest (first path component) */
static bool split_first_component(const char *path, char **first,
                                  char **rest)
{
    if (!path || *path != '/')
        return false;
    const char *p = path + 1;
    const char *slash = strchr(p, '/');
    if (!slash) {
        *first = gf_strdup(p);
        *rest = NULL;
        return *first[0] != '\0';
    }
    size_t fl = (size_t)(slash - p);
    if (fl == 0)
        return false;
    *first = gf_strndup(p, fl);
    *rest = gf_strdup(slash + 1);
    return true;
}

static int parse_url(gf_pkgref *out, const char *url)
{
    /* accept git+https:// and https:// (http:// rejected earlier) */
    const char *p = url;
    if (gf_str_starts_with(p, "git+"))
        p += 4;
    if (!gf_str_starts_with(p, "https://")) {
        gf_log(GF_LOG_ERROR,
               "refusing non-https repository URL: %s (use https://)", url);
        return -1;
    }
    p += 8; /* after https:// */
    char *copy = gf_strdup(p);
    strip_git_suffix(copy);
    char *slash = strchr(copy, '/');
    if (!slash) {
        gf_log(GF_LOG_ERROR, "URL has no repository path: %s", url);
        free(copy);
        return -1;
    }
    out->host = gf_strndup(copy, (size_t)(slash - copy));
    char *path = slash; /* keep the leading '/' for component split */
    char *owner = NULL, *rest = NULL;
    if (split_first_component(path, &owner, &rest) && rest) {
        out->owner = owner;
        out->repo = gf_strdup(rest);
        /* if repo has further components, keep only first (owner/repo) */
        char *rs = strchr(out->repo, '/');
        if (rs)
            *rs = '\0';
        free(rest);
    } else {
        free(owner);
        free(rest);
        out->repo = gf_strdup(path);
        char *rs = strchr(out->repo, '/');
        if (rs)
            *rs = '\0';
    }
    out->url = gf_strdup(url);
    free(copy);
    return 0;
}

int gf_pkgref_parse(const char *spec, const char *default_forge,
                    gf_pkgref *out)
{
    memset(out, 0, sizeof(*out));
    if (!spec || !*spec) {
        gf_log(GF_LOG_ERROR, "empty package identifier");
        return -1;
    }
    out->raw = gf_strdup(spec);
    char *base = gf_strdup(spec);

    /* Reject unsupported/unsafe schemes on the raw spec BEFORE any '@'
     * splitting ("git@host:repo" must not be mangled into a version). */
    if (gf_str_starts_with(spec, "ssh://") ||
        gf_str_starts_with(spec, "git@") ||
        gf_str_starts_with(spec, "http://") ||
        gf_str_starts_with(spec, "git+http://")) {
        gf_log(GF_LOG_ERROR,
               "unsupported repository URL form: %s "
               "(gitfull only speaks https)",
               spec);
        goto fail;
    }

    /* extract @version suffix (not inside URLs; '@' is rare there) */
    char *at = strrchr(base, '@');
    if (at && at != base && at[1] != '\0' && strstr(base, "://") == NULL &&
        !strchr(at + 1, '/')) {
        out->version = gf_strdup(at + 1);
        *at = '\0';
    }

    if (gf_str_starts_with(base, "local:")) {
        out->local = true;
        out->local_path = gf_strdup(base + 6);
        if (!*out->local_path) {
            gf_log(GF_LOG_ERROR, "local: reference needs a path");
            goto fail;
        }
        char *b = gf_path_basename(out->local_path);
        out->repo = b;
        free(base);
        return 0;
    }

    if (gf_str_starts_with(base, "git+https://") ||
        gf_str_starts_with(base, "https://")) {
        if (parse_url(out, base) != 0)
            goto fail;
        free(base);
        return 0;
    }

    /* forge-prefixed: github:owner/repo */
    char *colon = strchr(base, ':');
    if (colon && colon != base) {
        char *prefix = gf_strndup(base, (size_t)(colon - base));
        const char *rest = colon + 1;
        if (!strchr(rest, ':')) { /* not a scheme — treat as forge prefix */
            if (!strchr(rest, '/')) {
                gf_log(GF_LOG_ERROR,
                       "forge reference needs owner/repo: %s", base);
                free(prefix);
                goto fail;
            }
            out->forge = prefix;
            char *slash = strchr(rest, '/');
            out->owner = gf_strndup(rest, (size_t)(slash - rest));
            out->repo = gf_strdup(slash + 1);
            char *rs = strchr(out->repo, '/');
            if (rs)
                *rs = '\0';
            if (!out->owner[0] || !out->repo[0]) {
                gf_log(GF_LOG_ERROR, "malformed forge reference: %s", base);
                goto fail;
            }
            free(base);
            return 0;
        }
        free(prefix);
    }

    /* plain owner/repo */
    char *slash = strchr(base, '/');
    if (slash) {
        if (slash == base || slash[1] == '\0' || strchr(slash + 1, '/')) {
            gf_log(GF_LOG_ERROR, "malformed package reference: %s", spec);
            goto fail;
        }
        if (default_forge)
            out->forge = gf_strdup(default_forge);
        out->owner = gf_strndup(base, (size_t)(slash - base));
        out->repo = gf_strdup(slash + 1);
        free(base);
        return 0;
    }

    /* bare name — installed-package / recipe operations */
    out->repo = gf_strdup(base);
    if (default_forge)
        out->forge = gf_strdup(default_forge);
    free(base);
    return 0;

fail:
    free(base);
    return -1;
}

char *gf_pkgref_name(const gf_pkgref *r)
{
    if (r->local_path) {
        char *b = gf_path_basename(r->local_path);
        return b;
    }
    if (r->repo)
        return gf_strdup(r->repo);
    if (r->owner)
        return gf_strdup(r->owner);
    return gf_strdup(r->raw ? r->raw : "");
}

char *gf_pkgref_clone_url(const gf_pkgref *r, const char *web_base)
{
    if (!r)
        return NULL;
    if (r->url)
        return gf_strdup(r->url);
    if (r->owner && r->repo && web_base) {
        /* URL joining: strip trailing slashes only (NOT path normalization,
         * which would collapse the '//' after the scheme) */
        char *b = gf_strdup(web_base);
        size_t bl = strlen(b);
        while (bl > 0 && b[bl - 1] == '/')
            b[--bl] = '\0';
        gf_strbuf sb;
        gf_strbuf_init(&sb);
        gf_strbuf_appendf(&sb, "%s/%s/%s", b, r->owner, r->repo);
        free(b);
        return gf_strbuf_steal(&sb);
    }
    return NULL;
}
