/* detect.h — build system autodetection from repository contents. */
#ifndef GF_DETECT_H
#define GF_DETECT_H

#include <stdbool.h>

typedef enum {
    GF_BSYS_NONE = 0,
    GF_BSYS_CARGO,
    GF_BSYS_GO,
    GF_BSYS_NODE,
    GF_BSYS_PYTHON,
    GF_BSYS_MESON,
    GF_BSYS_CMAKE,
    GF_BSYS_AUTOTOOLS,
    GF_BSYS_MAKE,
    GF_BSYS_VALA,
    GF_BSYS_CUSTOM
} gf_bsys;

const char *gf_bsys_name(gf_bsys b);

/* Detect the build system from files in source_dir.
 * Priority follows ecosystem specificity: language manifests first (they
 * imply their toolchains), then meta-build systems, then raw Makefile. */
gf_bsys gf_bsys_detect(const char *source_dir);

/* Required host-independent tools for a build system (NULL-terminated list
 * of tool names that must exist in the toolchain), for doctor/toolchain. */
const char *const *gf_bsys_tools(gf_bsys b);

/* A short human description of how gitfull builds this system. */
const char *gf_bsys_description(gf_bsys b);

#endif /* GF_DETECT_H */
