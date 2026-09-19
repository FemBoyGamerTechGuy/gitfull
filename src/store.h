/* store.h — content-addressed package store. */
#ifndef GF_STORE_H
#define GF_STORE_H

#include "package.h"

#include <stdbool.h>
#include <stddef.h>

/* <state>/store/<name>/<version>/<build-id>/pkg.gfpkg */
char *gf_store_pkg_path(const char *state_dir, const char *name,
                        const char *version, const char *build_id);
/* <state>/store/<name>/<version>/<build-id>/files/ (payload) */
char *gf_store_files_path(const char *state_dir, const char *name,
                          const char *version, const char *build_id);

/* Install a .gfpkg into the store: verify + extract payload.
 * Returns 0/-1. Idempotent (existing identical build-id is accepted). */
int gf_store_put(const char *state_dir, const char *pkg_file,
                 const gf_pkgmeta *meta);

/* does this exact build exist? */
bool gf_store_has(const char *state_dir, const char *name,
                  const char *version, const char *build_id);

/* list versions present for a package (malloc'd strings, sorted newest
 * first by version comparison). */
char **gf_store_versions(const char *state_dir, const char *name, size_t *n);

/* remove one version's directory from the store (used by GC) */
int gf_store_remove_version(const char *state_dir, const char *name,
                            const char *version, const char *build_id);

/* total store usage in bytes */
uint64_t gf_store_usage(const char *state_dir);

#endif /* GF_STORE_H */
