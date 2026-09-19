/* seccomp.h — syscall filtering for sandboxed builds.
 *
 * A hand-built classic-BPF seccomp filter (no libseccomp dependency):
 *   - explicit ALLOW list for syscalls build toolchains need
 *   - explicit DENY list (EPERM) for privilege/escape vectors
 *   - everything else returns ENOSYS, which glibc/musl and most toolchains
 *     treat as "feature not available" and fall back gracefully
 *   - socket()/socketpair() are restricted to AF_UNIX when network
 *     isolation is requested (INET families return EAFNOSUPPORT)
 *
 * The filter is installed with PR_SET_NO_NEW_PRIVS, so setuid binaries
 * execed inside the sandbox cannot elevate. Filters are inherited by
 * children and cannot be removed — a build cannot shake it off.
 *
 * Availability: x86_64 only (the deny/allow tables use unistd numbers;
 * other architectures log a warning and continue without the filter).
 */
#ifndef GF_SECCOMP_H
#define GF_SECCOMP_H

#include <stdbool.h>

/* Policy knobs for the build sandbox. */
typedef struct gf_seccomp_policy {
    bool enabled;        /* install the filter at all */
    bool block_network;  /* restrict socket families to AF_UNIX */
} gf_seccomp_policy;

/* Can the kernel + this build of gitfull run seccomp filters?
 * (Cheap probe: PR_GET_SECCOMP must be understood.) */
bool gf_seccomp_supported(void);

/* Install the filter in the CURRENT process. Must be called after fork,
 * before exec, in a single-threaded process. Returns:
 *   0  — filter installed
 *   1  — not supported on this kernel/architecture (caller may continue,
 *        but should log that isolation is degraded)
 *  -1  — the filter could not be loaded (kernel refused; treat as
 *        infrastructure failure) */
int gf_seccomp_install(const gf_seccomp_policy *pol);

/* Human-readable summary for `gitfull doctor`. */
const char *gf_seccomp_describe(void);

#endif /* GF_SECCOMP_H */
