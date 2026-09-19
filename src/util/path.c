/* path.c — lexical path manipulation (no filesystem access).
 * All untrusted path validation flows through these helpers. */
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *gf_path_join(const char *a, const char *b)
{
    if (!a || !*a) {
        if (!b || !*b)
            return gf_strdup("/");
        return gf_strdup(b);
    }
    if (!b || !*b)
        return gf_strdup(a);
    while (*b == '/')
        b++; /* ignore leading slashes in b */
    if (!*b)
        return gf_strdup(a);
    bool slash = a[strlen(a) - 1] == '/';
    char *out = gf_malloc(strlen(a) + strlen(b) + 2);
    sprintf(out, "%s%s%s", a, slash ? "" : "/", b);
    return out;
}

char *gf_path_join_multi(const char *first, ...)
{
    va_list ap;
    va_start(ap, first);
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    const char *cur = first;
    while (cur) {
        if (*cur) {
            if (sb.len > 0 && sb.s[sb.len - 1] != '/' && *cur != '/')
                gf_strbuf_appendc(&sb, '/');
            gf_strbuf_append(&sb, cur);
        }
        cur = va_arg(ap, const char *);
    }
    va_end(ap);
    if (sb.len == 0)
        gf_strbuf_appendc(&sb, '/');
    char *out = gf_strbuf_steal(&sb);
    /* strip trailing slash (except root) */
    size_t l = strlen(out);
    if (l > 1 && out[l - 1] == '/')
        out[l - 1] = '\0';
    return out;
}

bool gf_path_is_abs(const char *path)
{
    return path && path[0] == '/';
}

char *gf_path_normalize(const char *path)
{
    if (!path)
        return gf_strdup("");
    bool abs = path[0] == '/';
    /* Tokenize into a stack of components. */
    char **parts = NULL;
    size_t nparts = 0, cap = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (nparts > 0) {
                free(parts[--nparts]);
            } else if (!abs) {
                /* keep leading ".." in relative paths */
                if (nparts == cap) {
                    cap = cap ? cap * 2 : 8;
                    parts = gf_realloc(parts, cap * sizeof(char *));
                }
                parts[nparts++] = gf_strdup("..");
            }
            /* absolute: ".." past root is dropped (cannot escape) */
            continue;
        }
        if (nparts == cap) {
            cap = cap ? cap * 2 : 8;
            parts = gf_realloc(parts, cap * sizeof(char *));
        }
        parts[nparts++] = gf_strndup(start, len);
    }

    gf_strbuf sb;
    gf_strbuf_init(&sb);
    if (abs)
        gf_strbuf_appendc(&sb, '/');
    for (size_t i = 0; i < nparts; i++) {
        if (i > 0)
            gf_strbuf_appendc(&sb, '/');
        gf_strbuf_append(&sb, parts[i]);
    }
    if (sb.len == 0)
        gf_strbuf_append(&sb, abs ? "" : ".");
    if (nparts > 0 && sb.len > 1 && sb.s[sb.len - 1] == '/')
        sb.s[--sb.len] = '\0';
    for (size_t i = 0; i < nparts; i++)
        free(parts[i]);
    free(parts);
    return gf_strbuf_steal(&sb);
}

bool gf_path_inside(const char *base, const char *path, bool allow_equal)
{
    if (!base || !path)
        return false;
    size_t lb = strlen(base);
    if (lb == 0)
        return false;
    bool base_slash = base[lb - 1] == '/';
    if (strncmp(path, base, lb) != 0)
        return false;
    if (strlen(path) == lb)
        return allow_equal;
    if (base_slash)
        return path[lb] != '\0'; /* everything below counts */
    return path[lb] == '/';
}

char *gf_path_dirname(const char *path)
{
    if (!path || !*path)
        return gf_strdup(".");
    char *norm = gf_path_normalize(path);
    char *slash = strrchr(norm, '/');
    if (!slash) {
        free(norm);
        return gf_strdup(".");
    }
    if (slash == norm) {
        free(norm);
        return gf_strdup("/");
    }
    *slash = '\0';
    char *out = gf_strdup(norm);
    free(norm);
    return out;
}

char *gf_path_basename(const char *path)
{
    if (!path || !*path)
        return gf_strdup("");
    char *norm = gf_path_normalize(path);
    char *slash = strrchr(norm, '/');
    char *out;
    if (!slash)
        out = gf_strdup(norm);
    else
        out = gf_strdup(slash + 1);
    free(norm);
    return out;
}

bool gf_path_component_ok(const char *name)
{
    if (!name || !*name)
        return false;
    if (strchr(name, '/'))
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    return true;
}
