/* ini.c — INI parsing for gitfull.conf. */
#include "ini.h"

#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kv {
    char *section; /* NULL for root */
    char *key;
    char *value;
};

struct gf_ini {
    struct kv *kvs;
    size_t len;
    size_t cap;
    char **sections; /* unique, ordered */
    size_t nsections;
};

static void ini_add(gf_ini *ini, char *section, char *key, char *value)
{
    if (ini->len == ini->cap) {
        ini->cap = ini->cap ? ini->cap * 2 : 16;
        ini->kvs = gf_realloc(ini->kvs, ini->cap * sizeof(struct kv));
    }
    ini->kvs[ini->len].section = section;
    ini->kvs[ini->len].key = key;
    ini->kvs[ini->len].value = value;
    ini->len++;

    if (section) {
        bool known = false;
        for (size_t i = 0; i < ini->nsections; i++) {
            if (strcmp(ini->sections[i], section) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            ini->sections = gf_realloc(ini->sections,
                                       (ini->nsections + 1) * sizeof(char *));
            ini->sections[ini->nsections++] = gf_strdup(section);
        }
    }
}

gf_ini *gf_ini_parse(const char *text, char *errbuf, size_t errlen)
{
    if (errbuf && errlen > 0)
        errbuf[0] = '\0';
    gf_ini *ini = gf_calloc(1, sizeof(gf_ini));
    char *cur_section = NULL;
    char *copy = gf_strdup(text);
    char *save = NULL;
    int lineno = 0;

    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        char *s = gf_str_trim(line);
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                if (errbuf)
                    snprintf(errbuf, errlen, "line %d: unclosed section header",
                             lineno);
                goto fail;
            }
            *end = '\0';
            char *name = gf_str_trim(s + 1);
            if (*name == '\0') {
                if (errbuf)
                    snprintf(errbuf, errlen, "line %d: empty section name",
                             lineno);
                goto fail;
            }
            free(cur_section);
            cur_section = gf_strdup(name);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            if (errbuf)
                snprintf(errbuf, errlen, "line %d: expected key = value",
                         lineno);
            goto fail;
        }
        *eq = '\0';
        char *key = gf_str_trim(s);
        char *val = gf_str_trim(eq + 1);
        if (*key == '\0') {
            if (errbuf)
                snprintf(errbuf, errlen, "line %d: empty key", lineno);
            goto fail;
        }
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
            val[vlen - 1] = '\0';
            val++;
        }
        ini_add(ini, cur_section ? gf_strdup(cur_section) : NULL,
                gf_strdup(key), gf_strdup(val));
    }
    free(copy);
    free(cur_section);
    return ini;

fail:
    free(copy);
    free(cur_section);
    gf_ini_free(ini);
    return NULL;
}

gf_ini *gf_ini_parse_file(const char *path, char *errbuf, size_t errlen)
{
    if (errbuf && errlen > 0)
        errbuf[0] = '\0';
    size_t len = 0;
    char *text = gf_fs_read_file_limit(path, 1 << 20, &len);
    if (!text) {
        if (errbuf)
            snprintf(errbuf, errlen, "cannot read %s", path);
        return NULL;
    }
    gf_ini *ini = gf_ini_parse(text, errbuf, errlen);
    free(text);
    return ini;
}

void gf_ini_free(gf_ini *ini)
{
    if (!ini)
        return;
    for (size_t i = 0; i < ini->len; i++) {
        free(ini->kvs[i].section);
        free(ini->kvs[i].key);
        free(ini->kvs[i].value);
    }
    free(ini->kvs);
    for (size_t i = 0; i < ini->nsections; i++)
        free(ini->sections[i]);
    free(ini->sections);
    free(ini);
}

const char *gf_ini_get(const gf_ini *ini, const char *section, const char *key)
{
    if (!ini)
        return NULL;
    for (size_t i = 0; i < ini->len; i++) {
        bool sec_eq = (section == NULL && ini->kvs[i].section == NULL) ||
                      (section != NULL && ini->kvs[i].section != NULL &&
                       strcmp(ini->kvs[i].section, section) == 0);
        if (!sec_eq)
            continue;
        if (strcmp(ini->kvs[i].key, key) == 0)
            return ini->kvs[i].value;
    }
    return NULL;
}

const char *gf_ini_get_def(const gf_ini *ini, const char *section,
                           const char *key, const char *def)
{
    const char *v = gf_ini_get(ini, section, key);
    return v ? v : def;
}

bool gf_ini_get_bool(const gf_ini *ini, const char *section, const char *key,
                     bool def)
{
    const char *v = gf_ini_get(ini, section, key);
    if (!v)
        return def;
    if (gf_str_ieq(v, "true") || gf_str_ieq(v, "yes") || gf_str_ieq(v, "1") ||
        gf_str_ieq(v, "on"))
        return true;
    if (gf_str_ieq(v, "false") || gf_str_ieq(v, "no") || gf_str_ieq(v, "0") ||
        gf_str_ieq(v, "off"))
        return false;
    return def;
}

size_t gf_ini_section_count(const gf_ini *ini)
{
    return ini ? ini->nsections : 0;
}

const char *gf_ini_section(const gf_ini *ini, size_t i)
{
    if (!ini || i >= ini->nsections)
        return NULL;
    return ini->sections[i];
}

size_t gf_ini_key_count(const gf_ini *ini, const char *section)
{
    if (!ini)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < ini->len; i++) {
        bool sec_eq = (section == NULL && ini->kvs[i].section == NULL) ||
                      (section != NULL && ini->kvs[i].section != NULL &&
                       strcmp(ini->kvs[i].section, section) == 0);
        if (sec_eq)
            n++;
    }
    return n;
}

const char *gf_ini_key(const gf_ini *ini, const char *section, size_t i)
{
    if (!ini)
        return NULL;
    size_t n = 0;
    for (size_t k = 0; k < ini->len; k++) {
        bool sec_eq = (section == NULL && ini->kvs[k].section == NULL) ||
                      (section != NULL && ini->kvs[k].section != NULL &&
                       strcmp(ini->kvs[k].section, section) == 0);
        if (!sec_eq)
            continue;
        if (n == i)
            return ini->kvs[k].key;
        n++;
    }
    return NULL;
}
