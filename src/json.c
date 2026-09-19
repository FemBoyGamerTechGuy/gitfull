/* json.c — strict recursive-descent JSON parser + canonical serializer. */
#include "json.h"

#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GF_JSON_MAX_DEPTH 64

struct parse_ctx {
    const char *p;
    const char *end;
    int depth;
    char *err;
    size_t errlen;
};

static void perr(struct parse_ctx *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void perr(struct parse_ctx *c, const char *fmt, ...)
{
    if (!c->err || c->errlen == 0)
        return;
    if (c->err[0])
        return; /* keep first error */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->errlen, fmt, ap);
    va_end(ap);
}

void gf_json_free(gf_json *j)
{
    if (!j)
        return;
    switch (j->type) {
    case GF_JSON_STRING:
        free(j->v.str);
        break;
    case GF_JSON_ARRAY:
        for (size_t i = 0; i < j->v.arr.len; i++)
            gf_json_free(j->v.arr.items[i]);
        free(j->v.arr.items);
        break;
    case GF_JSON_OBJECT:
        for (size_t i = 0; i < j->v.obj.len; i++) {
            free(j->v.obj.keys[i]);
            gf_json_free(j->v.obj.vals[i]);
        }
        free(j->v.obj.keys);
        free(j->v.obj.vals);
        break;
    case GF_JSON_NULL:
    case GF_JSON_BOOL:
    case GF_JSON_NUMBER:
    default:
        break;
    }
    free(j);
}

static void skip_ws(struct parse_ctx *c)
{
    while (c->p < c->end && (unsigned char)*c->p <= ' ')
        c->p++;
}

static gf_json *parse_value(struct parse_ctx *c);

static int utf8_encode(gf_strbuf *sb, uint32_t cp)
{
    if (cp < 0x80) {
        return gf_strbuf_appendc(sb, (char)cp);
    } else if (cp < 0x800) {
        if (gf_strbuf_appendc(sb, (char)(0xC0 | (cp >> 6))) != 0)
            return -1;
        return gf_strbuf_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        if (gf_strbuf_appendc(sb, (char)(0xE0 | (cp >> 12))) != 0)
            return -1;
        if (gf_strbuf_appendc(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) != 0)
            return -1;
        return gf_strbuf_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        if (gf_strbuf_appendc(sb, (char)(0xF0 | (cp >> 18))) != 0)
            return -1;
        if (gf_strbuf_appendc(sb, (char)(0x80 | ((cp >> 12) & 0x3F))) != 0)
            return -1;
        if (gf_strbuf_appendc(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) != 0)
            return -1;
        return gf_strbuf_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static bool hex4(struct parse_ctx *c, uint32_t *out)
{
    if (c->end - c->p < 4) {
        perr(c, "truncated \\u escape");
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = c->p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9')
            v |= (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            v |= (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            v |= (uint32_t)(ch - 'A' + 10);
        else {
            perr(c, "invalid hex digit in \\u escape");
            return false;
        }
    }
    c->p += 4;
    *out = v;
    return true;
}

static char *parse_string_raw(struct parse_ctx *c)
{
    if (c->p >= c->end || *c->p != '"') {
        perr(c, "expected string");
        return NULL;
    }
    c->p++;
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p;
        if ((unsigned char)ch < 0x20) {
            perr(c, "raw control character in string");
            goto fail;
        }
        if (ch != '\\') {
            c->p++;
            if (gf_strbuf_appendc(&sb, ch) != 0)
                goto fail;
            continue;
        }
        c->p++; /* skip backslash */
        if (c->p >= c->end) {
            perr(c, "truncated escape");
            goto fail;
        }
        char esc = *c->p++;
        switch (esc) {
        case '"':  if (gf_strbuf_appendc(&sb, '"') != 0) goto fail; break;
        case '\\': if (gf_strbuf_appendc(&sb, '\\') != 0) goto fail; break;
        case '/':  if (gf_strbuf_appendc(&sb, '/') != 0) goto fail; break;
        case 'b':  if (gf_strbuf_appendc(&sb, '\b') != 0) goto fail; break;
        case 'f':  if (gf_strbuf_appendc(&sb, '\f') != 0) goto fail; break;
        case 'n':  if (gf_strbuf_appendc(&sb, '\n') != 0) goto fail; break;
        case 'r':  if (gf_strbuf_appendc(&sb, '\r') != 0) goto fail; break;
        case 't':  if (gf_strbuf_appendc(&sb, '\t') != 0) goto fail; break;
        case 'u': {
            uint32_t cp;
            if (!hex4(c, &cp))
                goto fail;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* high surrogate: require following \uDC00-\uDFFF */
                if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                    c->p += 2;
                    uint32_t lo;
                    if (!hex4(c, &lo))
                        goto fail;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        perr(c, "invalid low surrogate");
                        goto fail;
                    }
                } else {
                    perr(c, "lone high surrogate");
                    goto fail;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                perr(c, "lone low surrogate");
                goto fail;
            }
            if (utf8_encode(&sb, cp) != 0)
                goto fail;
            break;
        }
        default:
            perr(c, "invalid escape character '\\%c'", esc);
            goto fail;
        }
    }
    if (c->p >= c->end) {
        perr(c, "unterminated string");
        goto fail;
    }
    c->p++; /* closing quote */
    return gf_strbuf_steal(&sb);
fail:
    gf_strbuf_free(&sb);
    return NULL;
}

static gf_json *parse_object(struct parse_ctx *c)
{
    c->p++; /* '{' */
    gf_json *obj = gf_json_new_object();
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return obj;
    }
    for (;;) {
        skip_ws(c);
        char *key = parse_string_raw(c);
        if (!key) {
            gf_json_free(obj);
            return NULL;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            perr(c, "expected ':' after object key");
            free(key);
            gf_json_free(obj);
            return NULL;
        }
        c->p++;
        skip_ws(c);
        gf_json *val = parse_value(c);
        if (!val) {
            free(key);
            gf_json_free(obj);
            return NULL;
        }
        if (gf_json_get(obj, key) != NULL) {
            perr(c, "duplicate object key '%s'", key);
            free(key);
            gf_json_free(val);
            gf_json_free(obj);
            return NULL;
        }
        gf_json_object_set(obj, key, val);
        free(key);
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return obj;
        }
        perr(c, "expected ',' or '}' in object");
        gf_json_free(obj);
        return NULL;
    }
}

static gf_json *parse_array(struct parse_ctx *c)
{
    c->p++; /* '[' */
    gf_json *arr = gf_json_new_array();
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return arr;
    }
    for (;;) {
        skip_ws(c);
        gf_json *item = parse_value(c);
        if (!item) {
            gf_json_free(arr);
            return NULL;
        }
        gf_json_array_push(arr, item);
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return arr;
        }
        perr(c, "expected ',' or ']' in array");
        gf_json_free(arr);
        return NULL;
    }
}

static gf_json *parse_number(struct parse_ctx *c)
{
    const char *start = c->p;
    if (c->p < c->end && *c->p == '-')
        c->p++;
    if (c->p >= c->end || !isdigit((unsigned char)*c->p)) {
        perr(c, "invalid number");
        return NULL;
    }
    if (*c->p == '0') {
        c->p++;
    } else {
        while (c->p < c->end && isdigit((unsigned char)*c->p))
            c->p++;
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (c->p >= c->end || !isdigit((unsigned char)*c->p)) {
            perr(c, "invalid number fraction");
            return NULL;
        }
        while (c->p < c->end && isdigit((unsigned char)*c->p))
            c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-'))
            c->p++;
        if (c->p >= c->end || !isdigit((unsigned char)*c->p)) {
            perr(c, "invalid number exponent");
            return NULL;
        }
        while (c->p < c->end && isdigit((unsigned char)*c->p))
            c->p++;
    }
    char buf[64];
    size_t n = (size_t)(c->p - start);
    if (n >= sizeof(buf)) {
        perr(c, "number too long");
        return NULL;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    errno = 0;
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (errno == ERANGE) {
        perr(c, "number out of range");
        return NULL;
    }
    (void)endp;
    return gf_json_new_num(v);
}

static gf_json *parse_value(struct parse_ctx *c)
{
    skip_ws(c);
    if (c->p >= c->end) {
        perr(c, "unexpected end of input");
        return NULL;
    }
    if (c->depth >= GF_JSON_MAX_DEPTH) {
        perr(c, "nesting too deep (limit %d)", GF_JSON_MAX_DEPTH);
        return NULL;
    }
    char ch = *c->p;
    switch (ch) {
    case '{':
        c->depth++;
        {
            gf_json *o = parse_object(c);
            c->depth--;
            return o;
        }
    case '[':
        c->depth++;
        {
            gf_json *a = parse_array(c);
            c->depth--;
            return a;
        }
    case '"': {
        char *s = parse_string_raw(c);
        if (!s)
            return NULL;
        gf_json *j = gf_malloc(sizeof(gf_json));
        j->type = GF_JSON_STRING;
        j->v.str = s;
        return j;
    }
    case 't':
        if (c->end - c->p >= 4 && memcmp(c->p, "true", 4) == 0) {
            c->p += 4;
            return gf_json_new_bool(true);
        }
        perr(c, "invalid literal");
        return NULL;
    case 'f':
        if (c->end - c->p >= 5 && memcmp(c->p, "false", 5) == 0) {
            c->p += 5;
            return gf_json_new_bool(false);
        }
        perr(c, "invalid literal");
        return NULL;
    case 'n':
        if (c->end - c->p >= 4 && memcmp(c->p, "null", 4) == 0) {
            c->p += 4;
            return gf_json_new_null();
        }
        perr(c, "invalid literal");
        return NULL;
    default:
        if (ch == '-' || isdigit((unsigned char)ch))
            return parse_number(c);
        perr(c, "unexpected character '%c'", ch);
        return NULL;
    }
}

gf_json *gf_json_parse(const char *text, size_t len, char *errbuf,
                       size_t errlen)
{
    if (errbuf && errlen > 0)
        errbuf[0] = '\0';
    if (!text) {
        if (errbuf)
            snprintf(errbuf, errlen, "NULL input");
        return NULL;
    }
    struct parse_ctx c = { text, text + len, 0, errbuf, errlen };
    gf_json *j = parse_value(&c);
    if (!j)
        return NULL;
    skip_ws(&c);
    if (c.p != c.end) {
        perr(&c, "trailing data after JSON value");
        gf_json_free(j);
        return NULL;
    }
    return j;
}

/* ------------------------------------------------------------- accessors */

const gf_json *gf_json_get(const gf_json *obj, const char *key)
{
    if (!obj || obj->type != GF_JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->v.obj.len; i++) {
        if (strcmp(obj->v.obj.keys[i], key) == 0)
            return obj->v.obj.vals[i];
    }
    return NULL;
}

const gf_json *gf_json_at(const gf_json *arr, size_t i)
{
    if (!arr || arr->type != GF_JSON_ARRAY || i >= arr->v.arr.len)
        return NULL;
    return arr->v.arr.items[i];
}

size_t gf_json_len(const gf_json *j)
{
    if (!j)
        return 0;
    if (j->type == GF_JSON_ARRAY)
        return j->v.arr.len;
    if (j->type == GF_JSON_OBJECT)
        return j->v.obj.len;
    return 0;
}

const char *gf_json_key(const gf_json *obj, size_t i)
{
    if (!obj || obj->type != GF_JSON_OBJECT || i >= obj->v.obj.len)
        return NULL;
    return obj->v.obj.keys[i];
}

const gf_json *gf_json_val(const gf_json *obj, size_t i)
{
    if (!obj || obj->type != GF_JSON_OBJECT || i >= obj->v.obj.len)
        return NULL;
    return obj->v.obj.vals[i];
}

const char *gf_json_str(const gf_json *j)
{
    if (!j || j->type != GF_JSON_STRING)
        return NULL;
    return j->v.str;
}

double gf_json_num(const gf_json *j, double def)
{
    if (!j || j->type != GF_JSON_NUMBER)
        return def;
    return j->v.num;
}

bool gf_json_bool(const gf_json *j, bool def)
{
    if (!j || j->type != GF_JSON_BOOL)
        return def;
    return j->v.b;
}

int64_t gf_json_int(const gf_json *j, int64_t def)
{
    if (!j || j->type != GF_JSON_NUMBER)
        return def;
    return (int64_t)j->v.num;
}

/* -------------------------------------------------------------- builders */

gf_json *gf_json_new_null(void)
{
    gf_json *j = gf_calloc(1, sizeof(gf_json));
    j->type = GF_JSON_NULL;
    return j;
}

gf_json *gf_json_new_bool(bool b)
{
    gf_json *j = gf_calloc(1, sizeof(gf_json));
    j->type = GF_JSON_BOOL;
    j->v.b = b;
    return j;
}

gf_json *gf_json_new_num(double v)
{
    gf_json *j = gf_calloc(1, sizeof(gf_json));
    j->type = GF_JSON_NUMBER;
    j->v.num = v;
    return j;
}

gf_json *gf_json_new_int(int64_t v)
{
    return gf_json_new_num((double)v);
}

gf_json *gf_json_new_string(const char *s)
{
    return gf_json_new_stringn(s, s ? strlen(s) : 0);
}

gf_json *gf_json_new_stringn(const char *s, size_t n)
{
    gf_json *j = gf_calloc(1, sizeof(gf_json));
    j->type = GF_JSON_STRING;
    j->v.str = gf_strndup(s ? s : "", s ? n : 0);
    return j;
}

gf_json *gf_json_new_array(void)
{
    gf_json *j = gf_calloc(1, sizeof(gf_json));
    j->type = GF_JSON_ARRAY;
    return j;
}

gf_json *gf_json_new_object(void)
{
    gf_json *j = gf_calloc(1, sizeof(gf_json));
    j->type = GF_JSON_OBJECT;
    return j;
}

int gf_json_array_push(gf_json *arr, gf_json *item)
{
    if (!arr || arr->type != GF_JSON_ARRAY) {
        gf_json_free(item);
        return -1;
    }
    size_t n = arr->v.arr.len;
    arr->v.arr.items = gf_realloc(arr->v.arr.items, (n + 1) * sizeof(gf_json *));
    arr->v.arr.items[n] = item;
    arr->v.arr.len = n + 1;
    return 0;
}

int gf_json_object_set(gf_json *obj, const char *key, gf_json *val)
{
    if (!obj || obj->type != GF_JSON_OBJECT || !key) {
        gf_json_free(val);
        return -1;
    }
    for (size_t i = 0; i < obj->v.obj.len; i++) {
        if (strcmp(obj->v.obj.keys[i], key) == 0) {
            gf_json_free(obj->v.obj.vals[i]);
            obj->v.obj.vals[i] = val;
            return 0;
        }
    }
    size_t n = obj->v.obj.len;
    obj->v.obj.keys = gf_realloc(obj->v.obj.keys, (n + 1) * sizeof(char *));
    obj->v.obj.vals = gf_realloc(obj->v.obj.vals, (n + 1) * sizeof(gf_json *));
    obj->v.obj.keys[n] = gf_strdup(key);
    obj->v.obj.vals[n] = val;
    obj->v.obj.len = n + 1;
    return 0;
}

/* ---------------------------------------------------------- serialization */

static int dump_str(gf_strbuf *sb, const char *s)
{
    if (gf_strbuf_appendc(sb, '"') != 0)
        return -1;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  if (gf_strbuf_append(sb, "\\\"") != 0) return -1; break;
        case '\\': if (gf_strbuf_append(sb, "\\\\") != 0) return -1; break;
        case '\b': if (gf_strbuf_append(sb, "\\b") != 0) return -1; break;
        case '\f': if (gf_strbuf_append(sb, "\\f") != 0) return -1; break;
        case '\n': if (gf_strbuf_append(sb, "\\n") != 0) return -1; break;
        case '\r': if (gf_strbuf_append(sb, "\\r") != 0) return -1; break;
        case '\t': if (gf_strbuf_append(sb, "\\t") != 0) return -1; break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                if (gf_strbuf_append(sb, esc) != 0)
                    return -1;
            } else {
                if (gf_strbuf_appendc(sb, (char)c) != 0)
                    return -1;
            }
        }
    }
    return gf_strbuf_appendc(sb, '"');
}

static int num_to_str(char *buf, size_t buflen, double v)
{
    /* shortest representation that round-trips */
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, buflen, "%.*g", prec, v);
        if (strtod(buf, NULL) == v)
            return 0;
    }
    snprintf(buf, buflen, "%.17g", v);
    return 0;
}

static int dump_value(gf_strbuf *sb, const gf_json *j, int indent, bool pretty)
{
    if (!j) {
        return gf_strbuf_append(sb, "null");
    }
    char nbuf[40];
    switch (j->type) {
    case GF_JSON_NULL:
        return gf_strbuf_append(sb, "null");
    case GF_JSON_BOOL:
        return gf_strbuf_append(sb, j->v.b ? "true" : "false");
    case GF_JSON_NUMBER:
        num_to_str(nbuf, sizeof(nbuf), j->v.num);
        return gf_strbuf_append(sb, nbuf);
    case GF_JSON_STRING:
        return dump_str(sb, j->v.str);
    case GF_JSON_ARRAY:
        if (j->v.arr.len == 0)
            return gf_strbuf_append(sb, "[]");
        if (gf_strbuf_append(sb, "[") != 0)
            return -1;
        for (size_t i = 0; i < j->v.arr.len; i++) {
            if (i > 0 && gf_strbuf_appendc(sb, ',') != 0)
                return -1;
            if (pretty && gf_strbuf_append(sb, "\n") != 0)
                return -1;
            if (pretty) {
                for (int k = 0; k < indent + 2; k++)
                    if (gf_strbuf_appendc(sb, ' ') != 0)
                        return -1;
            }
            if (dump_value(sb, j->v.arr.items[i], indent + 2, pretty) != 0)
                return -1;
        }
        if (pretty) {
            if (gf_strbuf_appendc(sb, '\n') != 0)
                return -1;
            for (int k = 0; k < indent; k++)
                if (gf_strbuf_appendc(sb, ' ') != 0)
                    return -1;
        }
        return gf_strbuf_appendc(sb, ']');
    case GF_JSON_OBJECT: {
        if (j->v.obj.len == 0)
            return gf_strbuf_append(sb, "{}");
        /* sorted key order for canonical output */
        size_t n = j->v.obj.len;
        size_t *idx = gf_malloc(n * sizeof(size_t));
        for (size_t i = 0; i < n; i++)
            idx[i] = i;
        for (size_t i = 0; i + 1 < n; i++) {
            for (size_t k = i + 1; k < n; k++) {
                if (strcmp(j->v.obj.keys[idx[i]], j->v.obj.keys[idx[k]]) > 0) {
                    size_t t = idx[i];
                    idx[i] = idx[k];
                    idx[k] = t;
                }
            }
        }
        if (gf_strbuf_append(sb, "{") != 0) {
            free(idx);
            return -1;
        }
        for (size_t i = 0; i < n; i++) {
            size_t e = idx[i];
            if (i > 0 && gf_strbuf_appendc(sb, ',') != 0) {
                free(idx);
                return -1;
            }
            if (pretty && gf_strbuf_appendc(sb, '\n') != 0) {
                free(idx);
                return -1;
            }
            if (pretty) {
                for (int k = 0; k < indent + 2; k++)
                    if (gf_strbuf_appendc(sb, ' ') != 0) {
                        free(idx);
                        return -1;
                    }
            }
            if (dump_str(sb, j->v.obj.keys[e]) != 0 ||
                gf_strbuf_appendc(sb, pretty ? ':' : ':') != 0 ||
                (pretty && gf_strbuf_appendc(sb, ' ') != 0) ||
                dump_value(sb, j->v.obj.vals[e], indent + 2, pretty) != 0) {
                free(idx);
                return -1;
            }
        }
        free(idx);
        if (pretty) {
            if (gf_strbuf_appendc(sb, '\n') != 0)
                return -1;
            for (int k = 0; k < indent; k++)
                if (gf_strbuf_appendc(sb, ' ') != 0)
                    return -1;
        }
        return gf_strbuf_appendc(sb, '}');
    }
    }
    return -1;
}

char *gf_json_dump(const gf_json *j)
{
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    if (dump_value(&sb, j, 0, false) != 0) {
        gf_strbuf_free(&sb);
        return NULL;
    }
    return gf_strbuf_steal(&sb);
}

char *gf_json_dump_pretty(const gf_json *j)
{
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    if (dump_value(&sb, j, 0, true) != 0) {
        gf_strbuf_free(&sb);
        return NULL;
    }
    return gf_strbuf_steal(&sb);
}
