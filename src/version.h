/* version.h — semantic version parsing and stable-release selection.
 *
 * gitfull's stable policy: `install <pkg>` resolves to the newest release
 * tag that parses as a stable semantic version (no prerelease component).
 * `--edge` bypasses this and uses the branch head instead.
 */
#ifndef GF_VERSION_H
#define GF_VERSION_H

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_version {
    int major;
    int minor;
    int patch;
    char *prerelease;  /* NULL when stable; e.g. "rc1" */
    char *raw;         /* original input (tag or string) */
} gf_version;

/* Parse a tag/string into a version. Accepts optional leading 'v'/'V' or
 * "release-" prefix; the remainder must be MAJOR[.MINOR[.PATCH]][-PRERELEASE].
 * Build metadata after '+' is ignored for comparison but retained in raw. */
bool gf_version_parse(const char *s, gf_version *out);
void gf_version_free(gf_version *v);
/* deep copy */
gf_version gf_version_copy(const gf_version *v);

/* semver 2.0.0 comparison (build metadata ignored). */
int gf_version_cmp(const gf_version *a, const gf_version *b);
bool gf_version_is_stable(const gf_version *v);

/* Is this tag usable as a stable release? (parses + no prerelease) */
bool gf_version_tag_stable(const char *tag);

/* Pick the newest stable (or, with allow_prerelease, newest overall) tag
 * from the list. Returns malloc'd copy of the winning tag, or NULL. */
char *gf_version_best_tag(char *const *tags, size_t n, bool allow_prerelease);

/* ------------- version constraint specs (recipes, dependency resolution) ---
 * Grammar:  [op]version   with op in { =, ==, >=, <=, >, <, ^, ~, ! } or empty
 * (empty = exact). Wildcards: "1.2.x", "1.x". A bare "name" without operator
 * is exact. Multiple constraints can be joined with ',' ("and"). */
typedef enum {
    GF_VC_EXACT = 0,
    GF_VC_GTE,
    GF_VC_LTE,
    GF_VC_GT,
    GF_VC_LT,
    GF_VC_NE,
    GF_VC_CARET,   /* compatible: same major, >= given */
    GF_VC_TILDE,   /* same major.minor, >= given */
    GF_VC_ANY
} gf_vc_op;

typedef struct gf_vc {
    gf_vc_op op;
    gf_version ver;
    bool has_minor;
    bool has_patch;
} gf_vc;

bool gf_vc_parse(const char *spec, gf_vc *out);
void gf_vc_free(gf_vc *c);
bool gf_vc_match(const gf_vc *c, const gf_version *v);
/* A set of constraints (comma-separated). */
typedef struct gf_vcset {
    gf_vc *cons;
    size_t n;
} gf_vcset;
bool gf_vcset_parse(const char *spec, gf_vcset *out);
void gf_vcset_free(gf_vcset *set);
bool gf_vcset_match(const gf_vcset *set, const gf_version *v);
bool gf_vcset_empty(const gf_vcset *set);
/* human readable form of the set (malloc'd) */
char *gf_vcset_str(const gf_vcset *set);

#endif /* GF_VERSION_H */
