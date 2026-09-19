/* mem.c — allocation helpers. OOM is fatal by policy (documented in common.h). */
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom_abort(size_t n)
{
    fprintf(stderr, "gitfull: fatal: out of memory (request %zu bytes)\n", n);
    abort();
}

void *gf_malloc(size_t n)
{
    void *p = malloc(n);
    if (!p && n > 0)
        oom_abort(n);
    return p;
}

void *gf_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p && n > 0 && size > 0)
        oom_abort(n * size);
    return p;
}

void *gf_realloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n > 0)
        oom_abort(n);
    return q;
}

char *gf_strdup(const char *s)
{
    if (!s)
        return NULL;
    char *d = strdup(s);
    if (!d)
        oom_abort(strlen(s) + 1);
    return d;
}

char *gf_strndup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d)
        oom_abort(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

void *gf_memdup(const void *p, size_t n)
{
    void *d = malloc(n);
    if (!d && n > 0)
        oom_abort(n);
    memcpy(d, p, n);
    return d;
}

void gf_free_ptr(void *p)
{
    free(p);
}
