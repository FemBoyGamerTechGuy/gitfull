/* str.c — strings, strbuf, splitting/joining, natural compare, URL encode. */
#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- strbuf */
void gf_strbuf_init(gf_strbuf *sb)
{
    sb->s = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void gf_strbuf_free(gf_strbuf *sb)
{
    free(sb->s);
    gf_strbuf_init(sb);
}

void gf_strbuf_reset(gf_strbuf *sb)
{
    sb->len = 0;
    if (sb->s)
        sb->s[0] = '\0';
}

char *gf_strbuf_str(const gf_strbuf *sb)
{
    if (sb->s)
        return sb->s;
    static char empty[1] = "";
    return empty;
}

char *gf_strbuf_steal(gf_strbuf *sb)
{
    char *s = sb->s ? sb->s : gf_strdup("");
    gf_strbuf_init(sb);
    return s;
}

static int sb_grow(gf_strbuf *sb, size_t need)
{
    if (sb->len + need + 1 <= sb->cap)
        return 0;
    size_t ncap = sb->cap ? sb->cap : 64;
    while (ncap < sb->len + need + 1)
        ncap *= 2;
    char *ns = realloc(sb->s, ncap);
    if (!ns)
        return -1;
    sb->s = ns;
    sb->cap = ncap;
    return 0;
}

int gf_strbuf_appendn(gf_strbuf *sb, const char *s, size_t n)
{
    if (!s || n == 0)
        return 0;
    if (sb_grow(sb, n) != 0) {
        gf_log(GF_LOG_ERROR, "strbuf: out of memory");
        return -1;
    }
    memcpy(sb->s + sb->len, s, n);
    sb->len += n;
    sb->s[sb->len] = '\0';
    return 0;
}

int gf_strbuf_append(gf_strbuf *sb, const char *s)
{
    return gf_strbuf_appendn(sb, s, s ? strlen(s) : 0);
}

int gf_strbuf_appendc(gf_strbuf *sb, char c)
{
    return gf_strbuf_appendn(sb, &c, 1);
}

int gf_strbuf_appendf(gf_strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return -1;
    }
    size_t un = (size_t)need;
    if (sb_grow(sb, un) != 0) {
        va_end(ap2);
        gf_log(GF_LOG_ERROR, "strbuf: out of memory");
        return -1;
    }
    vsnprintf(sb->s + sb->len, un + 1, fmt, ap2);
    va_end(ap2);
    sb->len += un;
    return 0;
}

int gf_strbuf_append_hex(gf_strbuf *sb, const uint8_t *bytes, size_t n)
{
    static const char hexdig[] = "0123456789abcdef";
    if (sb_grow(sb, 2 * n) != 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        sb->s[sb->len + 2 * i] = hexdig[bytes[i] >> 4];
        sb->s[sb->len + 2 * i + 1] = hexdig[bytes[i] & 0x0F];
    }
    sb->len += 2 * n;
    sb->s[sb->len] = '\0';
    return 0;
}

/* ------------------------------------------------------------- strings */
bool gf_str_eq(const char *a, const char *b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp(a, b) == 0;
}

static char lower_ascii(char c)
{
    return (char)tolower((unsigned char)c);
}

bool gf_str_ieq(const char *a, const char *b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (lower_ascii(*a) != lower_ascii(*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool gf_str_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
        return false;
    size_t lp = strlen(prefix);
    return strncmp(s, prefix, lp) == 0;
}

bool gf_str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return false;
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return false;
    return strcmp(s + ls - lx, suffix) == 0;
}

char *gf_str_trim(char *s)
{
    if (!s)
        return s;
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (*start == '\0') {
        s[0] = '\0';
        return s;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1)))
        end--;
    *end = '\0';
    memmove(s, start, (size_t)(end - start) + 1);
    return s;
}

int gf_str_naturalcmp(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (*pa || *pb) {
        if (isdigit(*pa) && isdigit(*pb)) {
            unsigned long va = 0, vb = 0;
            while (isdigit(*pa)) {
                va = va * 10 + (unsigned long)(*pa - '0');
                pa++;
            }
            while (isdigit(*pb)) {
                vb = vb * 10 + (unsigned long)(*pb - '0');
                pb++;
            }
            if (va != vb)
                return va < vb ? -1 : 1;
            continue;
        }
        if (*pa != *pb)
            return *pa < *pb ? -1 : 1;
        if (*pa == '\0')
            break;
        pa++;
        pb++;
    }
    return 0;
}

char **gf_str_split(const char *s, const char *delims, size_t *count)
{
    *count = 0;
    if (!s)
        return NULL;
    size_t cap = 8;
    char **out = gf_malloc(cap * sizeof(char *));
    const char *p = s;
    size_t dlen = strlen(delims);
    while (*p) {
        while (*p && memchr(delims, *p, dlen))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && !memchr(delims, *p, dlen))
            p++;
        if (*count == cap) {
            cap *= 2;
            out = gf_realloc(out, cap * sizeof(char *));
        }
        out[(*count)++] = gf_strndup(start, (size_t)(p - start));
    }
    return out;
}

void gf_strv_free(char **v, size_t count)
{
    if (!v)
        return;
    for (size_t i = 0; i < count; i++)
        free(v[i]);
    free(v);
}

char *gf_strv_join(char *const *v, size_t count, char sep)
{
    if (count == 0)
        return gf_strdup("");
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += strlen(v[i]) + 1;
    char *out = gf_malloc(total);
    char *w = out;
    for (size_t i = 0; i < count; i++) {
        size_t l = strlen(v[i]);
        memcpy(w, v[i], l);
        w += l;
        if (i + 1 < count)
            *w++ = sep;
    }
    *w = '\0';
    return out;
}

char *gf_url_encode(const char *s)
{
    static const char hexdig[] = "0123456789ABCDEF";
    size_t len = strlen(s);
    /* worst case: 3 output bytes per input byte */
    char *out = gf_malloc(len * 3 + 1);
    char *w = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *w++ = (char)c;
        } else {
            *w++ = '%';
            *w++ = hexdig[c >> 4];
            *w++ = hexdig[c & 0x0F];
        }
    }
    *w = '\0';
    return out;
}

char *gf_str_shell_quote(const char *s)
{
    /* POSIX single-quote quoting: 'it''s' style escaping. Every character
     * except the quote itself is literal inside single quotes; a quote is
     * closed, escaped, and reopened. Empty input yields ''. */
    size_t len = strlen(s);
    /* worst case: 2 quotes + 3 bytes per input quote char + NUL */
    char *out = gf_malloc(len * 3 + 3);
    char *w = out;
    *w++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *w++ = '\'';
            *w++ = '\\';
            *w++ = '\'';
            *w++ = '\'';
        } else {
            *w++ = s[i];
        }
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}
