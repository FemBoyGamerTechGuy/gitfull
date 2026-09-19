/* toolchain.h — build toolchain identity and bootstrap.
 *
 * Bootstrap trust boundary (documented honestly):
 *   gitfull cannot conjure a compiler from nothing. In "bootstrap" mode the
 *   tools from the host's standard locations are bind-mounted READ-ONLY into
 *   the build sandbox and every used tool's identity (path, resolved path,
 *   version, sha256) is recorded into the build manifest and verified at
 *   install time. Builds never execute arbitrary host paths outside the
 *   recorded set.
 *   In "pinned" mode gitfull builds toolchain components from pinned source
 *   revisions into <state>/toolchains/pinned/<name>-<version> using the same
 *   sandbox pipeline (extension point implemented; populating all components
 *   is a long-running, opt-in operation).
 */
#ifndef GF_TOOLCHAIN_H
#define GF_TOOLCHAIN_H

#include "config.h"
#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* One recorded tool identity. */
typedef struct gf_tool {
    char *name;        /* e.g. "cc", "make" */
    char *path;        /* sandbox path, e.g. /usr/bin/cc */
    char *source;      /* host path it resolves to (bootstrap) or pinned id */
    char *version;     /* first line of `tool --version` */
    char *sha256;      /* hash of the binary (regular files only) */
} gf_tool;

typedef struct gf_toolchain {
    char *mode;              /* bootstrap | pinned */
    char *id;                /* sha256 of the manifest (stable identity) */
    char *dir;               /* directory exposed at /toolchain (may hold
                                manifest.json; bootstrap binds system dirs) */
    gf_tool *tools;
    size_t ntools;
    /* host dirs to bind read-only into the sandbox (bootstrap mode) */
    char **system_dirs;
    size_t nsystem_dirs;
} gf_toolchain;

/* Probe the toolchain for the tools a build system needs (plus a base set).
 * Populates the toolchain with recorded identities. Tools that are missing
 * are simply absent (the build will fail with a clear message when used).
 * mode: cfg->toolchain_mode. Returns 0/-1. */
int gf_toolchain_probe(const gf_config *cfg, const char *build_system,
                       gf_toolchain *tc);

void gf_toolchain_free(gf_toolchain *tc);

/* JSON manifest of the toolchain (deterministic, sorted). */
gf_json *gf_toolchain_manifest(const gf_toolchain *tc);

/* Lookup a recorded tool. */
const gf_tool *gf_toolchain_get(const gf_toolchain *tc, const char *name);

#endif /* GF_TOOLCHAIN_H */
