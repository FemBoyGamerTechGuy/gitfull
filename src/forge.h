/* forge.h — forge provider abstraction.
 *
 * A forge is a code-hosting service (GitHub, GitLab, Gitea/Codeberg, or a
 * generic Git server). Providers implement a vtable; the resolver and source
 * pipeline only talk to this interface, so new forges can be added without
 * touching resolution logic.
 */
#ifndef GF_FORGE_H
#define GF_FORGE_H

#include "config.h"
#include "http.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gf_forge gf_forge;

struct gf_forge {
    const gf_repo *repo;      /* owning repository configuration */
    char *token;              /* resolved token or NULL */
    gf_http_opts http;        /* http options (timeouts etc.) */
    const struct gf_forge_ops *ops;
};

struct gf_forge_ops {
    const char *kind;
    /* Repo metadata; default_branch may be NULL when unknown. */
    int (*repo_info)(gf_forge *f, const char *owner, const char *repo,
                     char **default_branch, char **description,
                     char **license);
    /* Published releases. May be NULL when the forge has no release API. */
    int (*list_releases)(gf_forge *f, const char *owner, const char *repo,
                         gf_json **out); /* JSON array, caller frees */
    /* All tags (JSON array of {name, commit}). */
    int (*list_tags)(gf_forge *f, const char *owner, const char *repo,
                     gf_json **out);
    /* HEAD commit sha of a branch (41-byte buffer). */
    int (*branch_head)(gf_forge *f, const char *owner, const char *repo,
                       const char *branch, char *sha_out);
    /* Tarball URL for a tag (malloc'd) or NULL. */
    char *(*tarball_url)(gf_forge *f, const char *owner, const char *repo,
                         const char *tag);
    /* Release assets for a tag (JSON array) — NULL fn means none. */
    int (*release_assets)(gf_forge *f, const char *owner, const char *repo,
                          const char *tag, gf_json **out);
    /* Search repositories; JSON array of hits. NULL fn means unsupported. */
    int (*search)(gf_forge *f, const char *query, gf_json **out);
};

/* Open a forge for a repository config. Returns NULL for unknown kinds. */
gf_forge *gf_forge_open(const gf_repo *repo, const gf_config *cfg);
void gf_forge_close(gf_forge *f);

/* GET an API path (absolute https URL) and parse JSON, with 429/5xx backoff.
 * The token is attached only when the URL host is the forge API host. */
int gf_forge_api_get_json(gf_forge *f, const char *url, gf_json **out);

/* -------- helpers to normalize release lists across providers --------
 * Each item: tag, prerelease(bool), draft(bool), published_at, assets[]. */
typedef struct gf_rel {
    char *tag;
    bool prerelease;
    bool draft;
    char *published_at;
    char *commit;      /* may be NULL; resolved separately */
    gf_json *assets;   /* owned array of asset objects (may be empty) */
} gf_rel;

/* Convert a provider JSON releases array to gf_rel list. */
int gf_forge_rels_from_json(const gf_json *arr, gf_rel **out, size_t *n);
void gf_forge_rels_free(gf_rel *rels, size_t n);

/* Select the newest stable release (or prerelease when allowed). */
int gf_forge_select_release(const gf_rel *rels, size_t n, bool allow_prerelease,
                            const gf_rel **out);
/* Find a release by exact tag. */
const gf_rel *gf_forge_find_release(const gf_rel *rels, size_t n,
                                    const char *tag);

/* Providers register their ops here (implemented in forge_*.c). */
extern const struct gf_forge_ops gf_forge_ops_github;
extern const struct gf_forge_ops gf_forge_ops_gitlab;
extern const struct gf_forge_ops gf_forge_ops_gitea;
extern const struct gf_forge_ops gf_forge_ops_generic;

#endif /* GF_FORGE_H */
