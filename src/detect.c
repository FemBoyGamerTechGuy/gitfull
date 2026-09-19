/* detect.c — build system autodetection.
 *
 * Repositories are NOT required to contain gitfull-specific files. Detection
 * inspects well-known manifest files; a gitfull.toml recipe may override the
 * result (handled by the caller).
 */
#include "detect.h"

#include "common.h"

#include <stdlib.h>
#include <string.h>

const char *gf_bsys_name(gf_bsys b)
{
    switch (b) {
    case GF_BSYS_CARGO:     return "cargo";
    case GF_BSYS_GO:        return "go";
    case GF_BSYS_NODE:      return "node";
    case GF_BSYS_PYTHON:    return "python";
    case GF_BSYS_MESON:     return "meson";
    case GF_BSYS_CMAKE:     return "cmake";
    case GF_BSYS_AUTOTOOLS: return "autotools";
    case GF_BSYS_MAKE:      return "make";
    case GF_BSYS_VALA:      return "vala";
    case GF_BSYS_CUSTOM:    return "custom";
    case GF_BSYS_NONE:
    default:                return "none";
    }
}

static bool has(const char *dir, const char *name)
{
    char *p = gf_path_join(dir, name);
    bool ok = gf_fs_exists(p);
    free(p);
    return ok;
}

gf_bsys gf_bsys_detect(const char *source_dir)
{
    /* 1. language ecosystems */
    if (has(source_dir, "Cargo.toml"))
        return GF_BSYS_CARGO;
    if (has(source_dir, "go.mod"))
        return GF_BSYS_GO;
    if (has(source_dir, "package.json"))
        return GF_BSYS_NODE;
    if (has(source_dir, "pyproject.toml") || has(source_dir, "setup.py"))
        return GF_BSYS_PYTHON;

    /* 2. Vala projects: meson.build + *.vala sources or vapi/ */
    {
        char *meson = gf_path_join(source_dir, "meson.build");
        bool meson_ok = gf_fs_is_file(meson);
        free(meson);
        size_t n = 0;
        char **entries = NULL;
        bool has_vala = false;
        if (gf_fs_list_dir(source_dir, &entries, &n) == 0) {
            for (size_t i = 0; i < n; i++) {
                if (gf_str_ends_with(entries[i], ".vala"))
                    has_vala = true;
                free(entries[i]);
            }
            free(entries);
        }
        if (has_vala) {
            /* Vala builds typically drive meson or autotools; record vala
             * as the system and use meson when present. */
            if (meson_ok)
                return GF_BSYS_VALA;
        }
    }

    /* 3. meta build systems */
    if (has(source_dir, "meson.build"))
        return GF_BSYS_MESON;
    if (has(source_dir, "CMakeLists.txt"))
        return GF_BSYS_CMAKE;

    /* 4. autotools: generated configure first, then configure.ac */
    if (has(source_dir, "configure"))
        return GF_BSYS_AUTOTOOLS;
    if (has(source_dir, "configure.ac") || has(source_dir, "configure.in")) {
        /* needs autoreconf — treated as autotools */
        return GF_BSYS_AUTOTOOLS;
    }
    if (has(source_dir, "autogen.sh"))
        return GF_BSYS_AUTOTOOLS;

    /* 5. plain Makefile (Makefile, GNUmakefile, makefile) */
    if (has(source_dir, "Makefile") || has(source_dir, "GNUmakefile") ||
        has(source_dir, "makefile"))
        return GF_BSYS_MAKE;

    return GF_BSYS_NONE;
}

const char *const *gf_bsys_tools(gf_bsys b)
{
    static const char *cargo[] = { "cargo", "rustc", NULL };
    static const char *go[] = { "go", NULL };
    static const char *node[] = { "node", "npm", NULL };
    static const char *python[] = { "python3", "pip3", NULL };
    static const char *meson[] = { "meson", "ninja", "pkg-config", "cc", NULL };
    static const char *cmake[] = { "cmake", "ninja", "make", "cc", NULL };
    static const char *autotools[] = { "sh", "make", "cc", "pkg-config", NULL };
    static const char *make[] = { "make", "cc", NULL };
    static const char *vala[] = { "valac", "meson", "ninja", "cc", NULL };
    static const char *custom[] = { "sh", NULL };
    static const char *none[] = { NULL };
    switch (b) {
    case GF_BSYS_CARGO:     return cargo;
    case GF_BSYS_GO:        return go;
    case GF_BSYS_NODE:      return node;
    case GF_BSYS_PYTHON:    return python;
    case GF_BSYS_MESON:     return meson;
    case GF_BSYS_CMAKE:     return cmake;
    case GF_BSYS_AUTOTOOLS: return autotools;
    case GF_BSYS_MAKE:      return make;
    case GF_BSYS_VALA:      return vala;
    case GF_BSYS_CUSTOM:    return custom;
    case GF_BSYS_NONE:
    default:                return none;
    }
}

const char *gf_bsys_description(gf_bsys b)
{
    switch (b) {
    case GF_BSYS_CARGO:
        return "cargo build --release, install with cargo install --root staging";
    case GF_BSYS_GO:
        return "go build, install into staging via go install or copy";
    case GF_BSYS_NODE:
        return "npm ci --prefix, install into staging";
    case GF_BSYS_PYTHON:
        return "python -m build / pip wheel, install into staging with --prefix";
    case GF_BSYS_MESON:
        return "meson setup build, ninja, DESTDIR staging ninja install";
    case GF_BSYS_CMAKE:
        return "cmake -S . -B build, ninja/make, DESTDIR staging install";
    case GF_BSYS_AUTOTOOLS:
        return "./configure --prefix=..., make, DESTDIR staging make install";
    case GF_BSYS_MAKE:
        return "make, DESTDIR staging make install (prefix configurable)";
    case GF_BSYS_VALA:
        return "vala project driven through meson/ninja";
    case GF_BSYS_CUSTOM:
        return "gitfull.toml recipe steps";
    case GF_BSYS_NONE:
    default:
        return "unknown";
    }
}
