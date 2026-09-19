/* source.h — source resolution, acquisition, and verification.
 *
 * Resolution policy (stable by default):
 *   - exact version (@v1.2.3 or recipe pin_tag): that tag's commit
 *   - stable: newest non-prerelease release tag
 *   - --edge: head of the default branch (recorded as edge)
 * Acquisition: release tarball (with checksum verification when the forge
 * provides a digest) or exact-commit extraction from a git mirror.
 * Verification levels: verified (valid signature) > checksum (digest
 * matched) > unsigned (no signature/digest available; identity recorded) >
 * unverified > edge.
 */
#ifndef GF_SOURCE_H
#define GF_SOURCE_H

#include "config.h"
#include "forge.h"
#include "pkgid.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    GF_SRCST_VERIFIED = 0,
    GF_SRCST_CHECKSUM,
    GF_SRCST_UNSIGNED,
    GF_SRCST_UNVERIFIED,
    GF_SRCST_EDGE
} gf_src_status;

const char *gf_src_status_name(gf_src_status s);

typedef struct gf_source_id {
    char *forge;        /* repository config name */
    char *repo_url;     /* canonical clone URL */
    char *owner;
    char *repo;
    char *ref;          /* tag or branch used */
    char *commit;       /* 40-hex commit */
    char *tree;         /* git tree sha (or archive sha) */
    char *archive_sha;  /* sha256 of the tarball when downloaded */
    gf_src_status status;
    char *description;  /* release description when available */
    char *license;      /* license when discovered */
} gf_source_id;

void gf_source_id_free(gf_source_id *s);
gf_source_id *gf_source_id_copy(const gf_source_id *s);

typedef struct gf_resolved {
    gf_pkgref ref;          /* the (normalized) package reference */
    gf_source_id src;
    char *version;          /* resolved version string (tag or edge marker) */
    bool is_edge;
    bool already_installed; /* installed with same version */
} gf_resolved;

void gf_resolved_free(gf_resolved *r);

typedef struct gf_resolve_opts {
    bool edge;              /* --edge */
    bool prerelease;        /* allow prerelease tags */
    const char *exact_tag;  /* override: exact tag */
} gf_resolve_opts;

/* Resolve a package reference to an exact source identity. */
gf_resolved *gf_resolve(const gf_config *cfg, const gf_pkgref *ref,
                        const gf_resolve_opts *opts);

/* Acquire the resolved source into dest_dir (created, must not exist).
 * Uses the tarball cache + git mirror cache under state_dir. */
int gf_source_acquire(const gf_config *cfg, gf_resolved *r,
                      const char *dest_dir);

/* Verification helpers used by acquire(). */
int gf_source_verify_signature(const gf_config *cfg, const gf_resolved *r,
                               bool *signed_valid, bool *cannot_check);

#endif /* GF_SOURCE_H */
