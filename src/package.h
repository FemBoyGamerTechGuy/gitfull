/* package.h — the gitfull package format (.gfpkg) and its store.
 *
 * .gfpkg layout (a deterministic uncompressed ustar archive):
 *   METADATA     canonical JSON: name, version, arch, source identity,
 *                dependency closure, build identity, toolchain identity,
 *                description, license
 *   MANIFEST     canonical JSON array: {path,type,mode,size,sha256,target}
 *                for every payload entry
 *   CHECKSUMS    "sha256  <path>" lines (human-readable mirror of MANIFEST)
 *   payload/...  the actual files (staged with DESTDIR semantics)
 *
 * build_id == sha256 of the whole .gfpkg file (content address).
 * Determinism: sorted entries, uid/gid 0, mtime = SOURCE_DATE_EPOCH or 0,
 * sanitized modes (no setuid/setgid/world-write).
 *
 * Store layout:
 *   <state>/store/<name>/<version>/<build-id>/pkg.gfpkg
 *   <state>/store/<name>/<version>/<build-id>/files/   (payload, hardlink src)
 */
#ifndef GF_PACKAGE_H
#define GF_PACKAGE_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gf_manifest_entry {
    char *path;     /* payload-relative path (no "payload/" prefix) */
    char type;      /* 'f' file, 'd' dir, 'l' symlink, 'h' hardlink */
    uint32_t mode;
    uint64_t size;
    char *sha256;   /* file content hash (files only) */
    char *target;   /* symlink target */
} gf_manifest_entry;

typedef struct gf_pkgmeta {
    char *name;
    char *version;
    char *arch;
    char *forge;         /* repository config name (may be NULL) */
    char *repo_url;      /* canonical https URL of the source repo */
    char *commit;        /* exact commit sha (40 hex) or NULL */
    char *source_sha;    /* sha256 of the source tree/archive blob */
    char *source_status; /* verified | checksum | unsigned | unverified | edge */
    char *description;
    char *license;
    char *build_id;      /* sha256 of the .gfpkg */
    char *pkg_sha;       /* same as build_id (explicit) */
    char *build_identity; /* JSON string of the full build identity */
    char *toolchain_id;  /* sha256 of toolchain manifest */
    /* dependencies: arrays of {name, spec, kind} */
    char **dep_names;
    char **dep_specs;
    char **dep_kinds;    /* run | build */
    size_t ndeps;
} gf_pkgmeta;

/* Frees owned fields only (container may be embedded/stack). */
void gf_pkgmeta_free(gf_pkgmeta *m);
/* Frees owned fields AND a heap container. */
void gf_pkgmeta_free_heap(gf_pkgmeta *m);
gf_pkgmeta *gf_pkgmeta_copy(const gf_pkgmeta *m);
gf_json *gf_pkgmeta_to_json(const gf_pkgmeta *m);
gf_pkgmeta *gf_pkgmeta_from_json(const gf_json *obj);

/* Build a package from a staging directory (DESTDIR-style tree).
 * Metadata fields are copied. Writes <out>.gfpkg deterministically.
 * Returns the malloc'd build id (sha256 hex) or NULL. epoch: mtime stamp. */
char *gf_package_create(const char *staging_dir, const gf_pkgmeta *meta,
                        const char *out_path, uint64_t epoch,
                        gf_manifest_entry **manifest_out, size_t *nentries);

/* Parse an existing .gfpkg (validating structure; does NOT hash payload). */
int gf_package_open(const char *pkg_path, gf_pkgmeta **meta_out,
                    gf_manifest_entry **entries_out, size_t *nentries_out,
                    char **build_id_out);

/* Verify a .gfpkg: recompute build id, check MANIFEST sha256 of every file
 * against the extracted payload tree at verify_dir (payload root; NULL
 * skips per-file hashing). */
int gf_package_verify(const char *pkg_path, const char *verify_dir);

/* Extract payload files into dest_dir (hardlink-ready tree, "files/" layout). */
int gf_package_extract_payload(const char *pkg_path, const char *dest_dir);

void gf_manifest_free(gf_manifest_entry *entries, size_t n);

#endif /* GF_PACKAGE_H */
