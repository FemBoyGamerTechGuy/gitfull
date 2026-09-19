/* build.h — isolated build orchestration: source -> staged -> .gfpkg.
 *
 * Pipeline: acquire source -> recipe/detection -> toolchain probe ->
 * dependency payloads -> sandboxed build + test (unprivileged) -> staging
 * (DESTDIR) -> deterministic package -> verify -> store.
 */
#ifndef GF_BUILD_H
#define GF_BUILD_H

#include "config.h"
#include "db.h"
#include "package.h"
#include "source.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_build_dep {
    char *name;
    char *version;
    char *build_id;
} gf_build_dep;

typedef struct gf_build_opts {
    bool skip_tests;      /* override config */
    bool force;           /* rebuild even if build-id exists in store */
    gf_build_dep *deps;   /* dependency payloads to expose in the sandbox */
    size_t ndeps;
    uint64_t timeout_ms;  /* per-step timeout (0 = config default) */
} gf_build_opts;

typedef struct gf_build_result {
    char *build_id;    /* sha256 of the .gfpkg (content address) */
    char *pkg_path;    /* path inside the store */
    gf_pkgmeta *meta;
    char *build_dir;   /* logs: build.log, identity.json, steps.jsonl */
} gf_build_result;

/* Frees owned fields; the container struct itself is NOT freed. */
void gf_build_result_free(gf_build_result *r);

/* Build the resolved package. Returns 0 and fills *out (owned) on success.
 * Exit-code semantics: -1 generic failure, 5 build failure, 6 test failure
 * (mapped by the CLI to GF_EXIT_*). */
int gf_build_package(const gf_config *cfg, gf_db *db, gf_resolved *res,
                     const gf_build_opts *opts, gf_build_result *out);

#endif /* GF_BUILD_H */
