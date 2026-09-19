/* ini.h — minimal INI parser for /etc/gitfull.conf and repo configs. */
#ifndef GF_INI_H
#define GF_INI_H

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_ini gf_ini;

/* Parse INI text. Sections: [name] (dotted names allowed, kept verbatim).
 * Keys: key = value; comments start with '#' or ';'. Values are trimmed and
 * may be quoted with "..." (quotes stripped). Returns NULL on syntax error
 * with a message in errbuf (if provided). */
gf_ini *gf_ini_parse(const char *text, char *errbuf, size_t errlen);
gf_ini *gf_ini_parse_file(const char *path, char *errbuf, size_t errlen);
void gf_ini_free(gf_ini *ini);

/* section NULL refers to the top-level (section-less) area. */
const char *gf_ini_get(const gf_ini *ini, const char *section, const char *key);
/* get with default */
const char *gf_ini_get_def(const gf_ini *ini, const char *section,
                           const char *key, const char *def);
bool gf_ini_get_bool(const gf_ini *ini, const char *section, const char *key,
                     bool def);

/* Ordered iteration over sections (unique, in order of first appearance). */
size_t gf_ini_section_count(const gf_ini *ini);
const char *gf_ini_section(const gf_ini *ini, size_t i);

/* Ordered key iteration within a section. */
size_t gf_ini_key_count(const gf_ini *ini, const char *section);
const char *gf_ini_key(const gf_ini *ini, const char *section, size_t i);

#endif /* GF_INI_H */
