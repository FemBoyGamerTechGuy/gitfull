/* toml.h — minimal TOML subset parser producing a gf_json tree.
 *
 * Supported: tables [a.b], array-of-tables [[a.b]], dotted keys, basic
 * strings with escapes, literal strings, integers, floats, booleans, arrays
 * (possibly nested, multi-line), inline tables {k = v}, comments.
 * Not supported (rejected): multi-line """ strings, dates, heterogeneous
 * deep nesting beyond sane limits. Enough for gitfull.toml recipes.
 */
#ifndef GF_TOML_H
#define GF_TOML_H

#include "json.h"

#include <stddef.h>

/* Parse TOML text into a JSON object tree (tables become objects, arrays of
 * tables become arrays of objects). Returns NULL with errbuf on failure. */
gf_json *gf_toml_parse(const char *text, size_t len, char *errbuf,
                       size_t errlen);

#endif /* GF_TOML_H */
