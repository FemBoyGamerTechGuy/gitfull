/* toml.c — TOML subset parser (see toml.h for the supported grammar). */
#include "toml.h"

#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mutable lookup without const casts */
static gf_json *json_get_mut(gf_json *obj, const char *key)
{
    if (!obj || obj->type != GF_JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->v.obj.len; i++) {
        if (strcmp(obj->v.obj.keys[i], key) == 0)
            return obj->v.obj.vals[i];
    }
    return NULL;
}

struct tctx {
    const char *p;
    const char *end;
    int line;
    char *err;
    size_t errlen;
};

static void terr(struct tctx *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void terr(struct tctx *c, const char *fmt, ...)
{
    if (!c->err || c->errlen == 0 || c->err[0])
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->errlen, fmt, ap);
    va_end(ap);
}

static void skip_ws(struct tctx *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t'))
        c->p++;
}

static void skip_ws_nl_comments(struct tctx *c)
{
    for (;;) {
        while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
            if (*c->p == '\n')
                c->line++;
            c->p++;
        }
        if (c->p < c->end && *c->p == '#') {
            while (c->p < c->end && *c->p != '\n')
                c->p++;
            continue;
        }
        break;
    }
}

/* expect end-of-line (optional trailing comment) */
static void expect_eol(struct tctx *c)
{
    skip_ws(c);
    if (c->p < c->end && *c->p == '#') {
        while (c->p < c->end && *c->p != '\n')
            c->p++;
    }
    if (c->p == c->end)
        return;
    if (*c->p == '\r')
        c->p++;
    if (c->p < c->end && *c->p == '\n') {
        c->p++;
        c->line++;
        return;
    }
    terr(c, "line %d: unexpected trailing characters", c->line);
}

static char *parse_key_part(struct tctx *c)
{
    if (c->p >= c->end) {
        terr(c, "line %d: unexpected end of key", c->line);
        return NULL;
    }
    if (*c->p == '"') {
        c->p++;
        gf_strbuf sb;
        gf_strbuf_init(&sb);
        while (c->p < c->end && *c->p != '"') {
            char ch = *c->p;
            if (ch == '\\') {
                c->p++;
                if (c->p >= c->end)
                    break;
                char e = *c->p++;
                switch (e) {
                case 'n': gf_strbuf_appendc(&sb, '\n'); break;
                case 't': gf_strbuf_appendc(&sb, '\t'); break;
                case '"': gf_strbuf_appendc(&sb, '"'); break;
                case '\\': gf_strbuf_appendc(&sb, '\\'); break;
                case 'u': {
                    uint32_t cp = 0;
                    for (int i = 0; i < 4 && c->p < c->end; i++) {
                        char h = *c->p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                        else { terr(c, "line %d: bad \\u escape", c->line); goto fail; }
                    }
                    /* simplistic BMP encode */
                    if (cp < 0x80) gf_strbuf_appendc(&sb, (char)cp);
                    else if (cp < 0x800) {
                        gf_strbuf_appendc(&sb, (char)(0xC0 | (cp >> 6)));
                        gf_strbuf_appendc(&sb, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        gf_strbuf_appendc(&sb, (char)(0xE0 | (cp >> 12)));
                        gf_strbuf_appendc(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        gf_strbuf_appendc(&sb, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    terr(c, "line %d: invalid escape \\%c", c->line, e);
                    goto fail;
                }
            } else {
                gf_strbuf_appendc(&sb, ch);
                c->p++;
            }
        }
        if (c->p >= c->end || *c->p != '"') {
            terr(c, "line %d: unterminated quoted key", c->line);
            goto fail;
        }
        c->p++;
        return gf_strbuf_steal(&sb);
fail:
        gf_strbuf_free(&sb);
        return NULL;
    }
    const char *start = c->p;
    while (c->p < c->end && (isalnum((unsigned char)*c->p) || *c->p == '_' ||
                             *c->p == '-'))
        c->p++;
    if (start == c->p) {
        terr(c, "line %d: expected key", c->line);
        return NULL;
    }
    return gf_strndup(start, (size_t)(c->p - start));
}

/* dotted key path: a.b.c */
static char **parse_key_path(struct tctx *c, size_t *n)
{
    size_t cap = 4, cnt = 0;
    char **parts = gf_malloc(cap * sizeof(char *));
    for (;;) {
        char *part = parse_key_part(c);
        if (!part) {
            gf_strv_free(parts, cnt);
            return NULL;
        }
        if (cnt == cap) {
            cap *= 2;
            parts = gf_realloc(parts, cap * sizeof(char *));
        }
        parts[cnt++] = part;
        skip_ws(c);
        if (c->p < c->end && *c->p == '.') {
            c->p++;
            skip_ws(c);
            continue;
        }
        break;
    }
    *n = cnt;
    return parts;
}

static gf_json *parse_value(struct tctx *c, int depth);

static gf_json *parse_basic_string(struct tctx *c)
{
    /* current char is '"' */
    c->p++;
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p;
        if (ch == '\n') {
            terr(c, "line %d: newline in basic string", c->line);
            goto fail;
        }
        if (ch != '\\') {
            gf_strbuf_appendc(&sb, ch);
            c->p++;
            continue;
        }
        c->p++;
        if (c->p >= c->end)
            break;
        char e = *c->p++;
        switch (e) {
        case 'n': gf_strbuf_appendc(&sb, '\n'); break;
        case 't': gf_strbuf_appendc(&sb, '\t'); break;
        case 'r': gf_strbuf_appendc(&sb, '\r'); break;
        case 'b': gf_strbuf_appendc(&sb, '\b'); break;
        case 'f': gf_strbuf_appendc(&sb, '\f'); break;
        case '"': gf_strbuf_appendc(&sb, '"'); break;
        case '\\': gf_strbuf_appendc(&sb, '\\'); break;
        case 'u': {
            uint32_t cp = 0;
            for (int i = 0; i < 4 && c->p < c->end; i++) {
                char h = *c->p++;
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                else { terr(c, "line %d: bad \\u escape", c->line); goto fail; }
            }
            if (cp < 0x80) gf_strbuf_appendc(&sb, (char)cp);
            else if (cp < 0x800) {
                gf_strbuf_appendc(&sb, (char)(0xC0 | (cp >> 6)));
                gf_strbuf_appendc(&sb, (char)(0x80 | (cp & 0x3F)));
            } else {
                gf_strbuf_appendc(&sb, (char)(0xE0 | (cp >> 12)));
                gf_strbuf_appendc(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                gf_strbuf_appendc(&sb, (char)(0x80 | (cp & 0x3F)));
            }
            break;
        }
        default:
            terr(c, "line %d: invalid escape in string", c->line);
            goto fail;
        }
    }
    if (c->p >= c->end) {
        terr(c, "line %d: unterminated string", c->line);
        goto fail;
    }
    c->p++; /* closing quote */
    {
        char *stolen = gf_strbuf_steal(&sb);
        gf_json *j = gf_json_new_string(stolen);
        free(stolen); /* new_string copies; release the scratch buffer */
        return j;
    }
fail:
    gf_strbuf_free(&sb);
    return NULL;
}

static gf_json *parse_literal_string(struct tctx *c)
{
    /* current char is '\'' */
    c->p++;
    const char *start = c->p;
    while (c->p < c->end && *c->p != '\'') {
        if (*c->p == '\n') {
            terr(c, "line %d: newline in literal string", c->line);
            return NULL;
        }
        c->p++;
    }
    if (c->p >= c->end) {
        terr(c, "line %d: unterminated literal string", c->line);
        return NULL;
    }
    gf_json *j = gf_json_new_stringn(start, (size_t)(c->p - start));
    c->p++;
    return j;
}

static gf_json *parse_number(struct tctx *c)
{
    const char *start = c->p;
    if (c->p < c->end && (*c->p == '+' || *c->p == '-'))
        c->p++;
    bool isfloat = false;
    while (c->p < c->end && (isdigit((unsigned char)*c->p) || *c->p == '_')) {
        if (*c->p == '_') {
            c->p++;
            continue;
        }
        c->p++;
    }
    if (c->p < c->end && *c->p == '.') {
        isfloat = true;
        c->p++;
        while (c->p < c->end && (isdigit((unsigned char)*c->p) || *c->p == '_'))
            c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        isfloat = true;
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-'))
            c->p++;
        while (c->p < c->end && isdigit((unsigned char)*c->p))
            c->p++;
    }
    if (c->p < c->end && (isalpha((unsigned char)*c->p))) {
        terr(c, "line %d: invalid numeric literal", c->line);
        return NULL;
    }
    char buf[64];
    size_t n = (size_t)(c->p - start);
    if (n == 0 || n >= sizeof(buf)) {
        terr(c, "line %d: bad number", c->line);
        return NULL;
    }
    /* strip underscores */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (start[i] != '_')
            buf[w++] = start[i];
    }
    buf[w] = '\0';
    if (isfloat) {
        errno = 0;
        double d = strtod(buf, NULL);
        if (errno)
            return NULL;
        return gf_json_new_num(d);
    }
    errno = 0;
    long long v = strtoll(buf, NULL, 0);
    if (errno)
        return NULL;
    return gf_json_new_int((int64_t)v);
}

static gf_json *parse_array(struct tctx *c, int depth)
{
    c->p++; /* [ */
    gf_json *arr = gf_json_new_array();
    for (;;) {
        skip_ws_nl_comments(c);
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return arr;
        }
        gf_json *item = parse_value(c, depth + 1);
        if (!item) {
            gf_json_free(arr);
            return NULL;
        }
        gf_json_array_push(arr, item);
        skip_ws_nl_comments(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return arr;
        }
        gf_json_free(arr);
        terr(c, "line %d: expected ',' or ']' in array", c->line);
        return NULL;
    }
}

static gf_json *parse_inline_table(struct tctx *c, int depth)
{
    c->p++; /* { */
    gf_json *obj = gf_json_new_object();
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return obj;
    }
    for (;;) {
        skip_ws(c);
        size_t npath = 0;
        char **path = parse_key_path(c, &npath);
        if (!path) {
            gf_json_free(obj);
            return NULL;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != '=') {
            gf_strv_free(path, npath);
            gf_json_free(obj);
            terr(c, "line %d: expected '=' in inline table", c->line);
            return NULL;
        }
        c->p++;
        skip_ws(c);
        gf_json *val = parse_value(c, depth + 1);
        if (!val) {
            gf_strv_free(path, npath);
            gf_json_free(obj);
            return NULL;
        }
        /* nested dotted assignment inside inline table */
        gf_json *tgt = obj;
        for (size_t i = 0; i + 1 < npath; i++) {
            gf_json *sub = json_get_mut(tgt, path[i]);
            if (!sub || sub->type != GF_JSON_OBJECT) {
                sub = gf_json_new_object();
                gf_json_object_set(tgt, path[i], sub);
            }
            tgt = sub;
        }
        gf_json_object_set(tgt, path[npath - 1], val);
        gf_strv_free(path, npath);
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return obj;
        }
        gf_json_free(obj);
        terr(c, "line %d: expected ',' or '}' in inline table", c->line);
        return NULL;
    }
}

static gf_json *parse_value(struct tctx *c, int depth)
{
    if (depth > 32) {
        terr(c, "nesting too deep");
        return NULL;
    }
    skip_ws(c);
    if (c->p >= c->end) {
        terr(c, "line %d: expected a value", c->line);
        return NULL;
    }
    char ch = *c->p;
    if (ch == '"')
        return parse_basic_string(c);
    if (ch == '\'')
        return parse_literal_string(c);
    if (ch == '[')
        return parse_array(c, depth);
    if (ch == '{')
        return parse_inline_table(c, depth);
    if (gf_str_starts_with(c->p, "true") && (c->p + 4 == c->end || !isalnum((unsigned char)c->p[4]))) {
        c->p += 4;
        return gf_json_new_bool(true);
    }
    if (gf_str_starts_with(c->p, "false") && (c->p + 5 == c->end || !isalnum((unsigned char)c->p[5]))) {
        c->p += 5;
        return gf_json_new_bool(false);
    }
    if (ch == '-' || ch == '+' || isdigit((unsigned char)ch))
        return parse_number(c);
    terr(c, "line %d: unexpected character '%c' in value", c->line, ch);
    return NULL;
}

/* navigate/creating objects along a path; returns the table or NULL. */
static gf_json *walk_table(gf_json *root, char **path, size_t n, bool array_last)
{
    gf_json *cur = root;
    for (size_t i = 0; i < n; i++) {
        bool last = (i + 1 == n);
        gf_json *next = json_get_mut(cur, path[i]);
        if (last && array_last) {
            /* array of tables: get or create array, then append a new object */
            if (!next) {
                next = gf_json_new_array();
                gf_json_object_set(cur, path[i], next);
            }
            if (next->type != GF_JSON_ARRAY) {
                terr(NULL, "key %s is not an array of tables", path[i]);
                return NULL;
            }
            gf_json *obj = gf_json_new_object();
            gf_json_array_push(next, obj);
            return obj;
        }
        if (!next) {
            next = gf_json_new_object();
            gf_json_object_set(cur, path[i], next);
        }
        if (next->type == GF_JSON_ARRAY) {
            /* descend into the last table of an array of tables */
            if (next->v.arr.len == 0) {
                gf_json *obj = gf_json_new_object();
                gf_json_array_push(next, obj);
            }
            next = next->v.arr.items[next->v.arr.len - 1];
        }
        if (next->type != GF_JSON_OBJECT) {
            return NULL;
        }
        cur = next;
    }
    return cur;
}

gf_json *gf_toml_parse(const char *text, size_t len, char *errbuf,
                       size_t errlen)
{
    if (errbuf && errlen > 0)
        errbuf[0] = '\0';
    struct tctx c = { text, text + len, 1, errbuf, errlen };
    gf_json *root = gf_json_new_object();
    gf_json *cur = root;

    for (;;) {
        skip_ws_nl_comments(&c);
        if (c.p >= c.end)
            break;
        if (*c.p == '[') {
            bool array_tbl = false;
            c.p++;
            if (c.p < c.end && *c.p == '[') {
                array_tbl = true;
                c.p++;
            }
            skip_ws(&c);
            size_t npath = 0;
            char **path = parse_key_path(&c, &npath);
            if (!path) {
                gf_json_free(root);
                return NULL;
            }
            skip_ws(&c);
            if (c.p >= c.end || *c.p != ']') {
                gf_strv_free(path, npath);
                gf_json_free(root);
                terr(&c, "line %d: expected ']'", c.line);
                return NULL;
            }
            c.p++;
            if (array_tbl) {
                if (c.p >= c.end || *c.p != ']') {
                    gf_strv_free(path, npath);
                    gf_json_free(root);
                    terr(&c, "line %d: expected ']]'", c.line);
                    return NULL;
                }
                c.p++;
            }
            expect_eol(&c);
            gf_json *tbl = walk_table(root, path, npath, array_tbl);
            gf_strv_free(path, npath);
            if (!tbl) {
                gf_json_free(root);
                if (errbuf && errlen > 0 && !errbuf[0])
                    snprintf(errbuf, errlen, "line %d: invalid table path", c.line);
                return NULL;
            }
            cur = tbl;
            continue;
        }
        /* key = value */
        size_t npath = 0;
        char **path = parse_key_path(&c, &npath);
        if (!path) {
            gf_json_free(root);
            return NULL;
        }
        skip_ws(&c);
        if (c.p >= c.end || *c.p != '=') {
            gf_strv_free(path, npath);
            gf_json_free(root);
            terr(&c, "line %d: expected '='", c.line);
            return NULL;
        }
        c.p++;
        skip_ws(&c);
        gf_json *val = parse_value(&c, 0);
        if (!val) {
            gf_strv_free(path, npath);
            gf_json_free(root);
            return NULL;
        }
        expect_eol(&c);
        gf_json *tgt = cur;
        for (size_t i = 0; i + 1 < npath; i++) {
            gf_json *sub = json_get_mut(tgt, path[i]);
            if (!sub) {
                sub = gf_json_new_object();
                gf_json_object_set(tgt, path[i], sub);
            }
            if (sub->type != GF_JSON_OBJECT) {
                sub = NULL;
                break;
            }
            tgt = sub;
        }
        if (tgt) {
            gf_json_object_set(tgt, path[npath - 1], val);
        } else {
            gf_json_free(val);
        }
        gf_strv_free(path, npath);
        if (c.err && c.err[0]) {
            gf_json_free(root);
            return NULL;
        }
    }
    return root;
}
