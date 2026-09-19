/* pkgid.h — package identifier parsing.
 *
 * Supported forms:
 *   owner/repository              (default forge, e.g. github)
 *   github:owner/repository       codeberg:...  gitlab:...  gitea:...
 *   git+https://host/owner/repo[.git]
 *   https://host/owner/repo[.git]
 *   local:/path/to/repo           (local git checkout or plain directory)
 *   name                          (installed package / local operation)
 * plus an optional exact version suffix:  <any of the above>@v1.2.3
 */
#ifndef GF_PKGID_H
#define GF_PKGID_H

#include <stdbool.h>

typedef struct gf_pkgref {
    char *raw;        /* original spec */
    char *forge;      /* configured repository name (github/gitlab/...) or
                         NULL when url was given directly and matched nothing */
    char *host;       /* forge host when known (e.g. github.com) */
    char *owner;      /* NULL when unknown */
    char *repo;       /* repo/dir name (also used as package name) */
    char *url;        /* full https/git URL when known, else NULL */
    char *local_path; /* set for local: refs; NULL otherwise */
    char *version;    /* version spec after '@' (without '@'), may be NULL */
    bool local;
} gf_pkgref;

/* Parse 'spec'. 'default_forge' names the default repository configuration
 * (usually "github"). Returns 0 on success, -1 with an error message logged.
 * Forms without '/' are accepted as bare names (repo=NULL) — callers decide
 * what those mean (installed lookup, etc.). */
int gf_pkgref_parse(const char *spec, const char *default_forge,
                    gf_pkgref *out);
void gf_pkgref_free(gf_pkgref *r);
gf_pkgref gf_pkgref_copy(const gf_pkgref *r);

/* Derive the human package name from a ref: repo basename without ".git",
 * or the local path's basename. malloc'd. */
char *gf_pkgref_name(const gf_pkgref *r);

/* Construct the canonical https clone URL for a forge ref, given the forge
 * web base (e.g. https://github.com). malloc'd. NULL when not derivable. */
char *gf_pkgref_clone_url(const gf_pkgref *r, const char *web_base);

#endif /* GF_PKGID_H */
