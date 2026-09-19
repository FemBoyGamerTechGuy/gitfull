/* json.h — minimal, strict JSON parser + canonical serializer.
 *
 * Rationale: gitfull must parse forge API responses and write deterministic
 * metadata files without pulling in a heavyweight dependency. This module is
 * intentionally small, allocation-checked, depth-limited and defensive.
 * It is covered by unit tests including malformed-input cases.
 */
#ifndef GF_JSON_H
#define GF_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GF_JSON_NULL = 0,
    GF_JSON_BOOL,
    GF_JSON_NUMBER,
    GF_JSON_STRING,
    GF_JSON_ARRAY,
    GF_JSON_OBJECT
} gf_json_type;

typedef struct gf_json gf_json;

struct gf_json {
    gf_json_type type;
    union {
        bool b;
        double num;
        char *str;
        struct {
            gf_json **items;
            size_t len;
        } arr;
        struct {
            char **keys;
            gf_json **vals;
            size_t len;
        } obj;
    } v;
};

/* Parse 'text' (need not be NUL-terminated; len given). Returns NULL on
 * error with a human-readable message in errbuf (if provided). */
gf_json *gf_json_parse(const char *text, size_t len, char *errbuf,
                       size_t errlen);
void gf_json_free(gf_json *j);

/* ---- read accessors (NULL-safe; wrong type yields default) ---- */
const gf_json *gf_json_get(const gf_json *obj, const char *key);
const gf_json *gf_json_at(const gf_json *arr, size_t i);
size_t gf_json_len(const gf_json *j);         /* array/object length */
const char *gf_json_key(const gf_json *obj, size_t i);
const gf_json *gf_json_val(const gf_json *obj, size_t i);
const char *gf_json_str(const gf_json *j);    /* NULL unless string */
double gf_json_num(const gf_json *j, double def);
bool gf_json_bool(const gf_json *j, bool def);
int64_t gf_json_int(const gf_json *j, int64_t def);

/* ---- builders (all returned values are owned by caller) ---- */
gf_json *gf_json_new_null(void);
gf_json *gf_json_new_bool(bool b);
gf_json *gf_json_new_num(double v);
gf_json *gf_json_new_int(int64_t v);
gf_json *gf_json_new_string(const char *s);
gf_json *gf_json_new_stringn(const char *s, size_t n);
gf_json *gf_json_new_array(void);
gf_json *gf_json_new_object(void);
/* Takes ownership of 'item' (transfers it into the array). */
int gf_json_array_push(gf_json *arr, gf_json *item);
/* Takes ownership of 'val'; replaces existing key if present. */
int gf_json_object_set(gf_json *obj, const char *key, gf_json *val);

/* ---- serialization ---- */
/* Canonical compact form: no whitespace, object keys sorted, deterministic
 * number formatting ("%.17g" trimmed). Deterministic across runs. */
char *gf_json_dump(const gf_json *j);
/* Pretty form (2-space indent); still key-sorted and deterministic. */
char *gf_json_dump_pretty(const gf_json *j);

#endif /* GF_JSON_H */
