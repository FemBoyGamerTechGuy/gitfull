/* sandbox.h — isolated build environments via Linux namespaces.
 *
 * Every package build runs inside its own mount/pid/user/uts/ipc (+optional
 * net) namespace with pivot_root into a prepared build root containing:
 *   /toolchain (ro)  /src (rw)  /build (rw)  /staging (rw)  /deps (ro)
 *   /home/build (rw tmpfs)  /tmp (tmpfs)  /dev (tmpfs + core nodes)
 *   /proc (procfs of the new pid namespace)  /etc (minimal static)
 *
 * Levels: userns (default, unprivileged), chroot (when running as real
 * root), none (degraded — refused unless explicitly configured).
 * Network: none (default; new netns with only loopback not yet up = no
 * external network) or fetch (host network shared for dependency fetches).
 */
#ifndef GF_SANDBOX_H
#define GF_SANDBOX_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GF_SANDBOX_USERNS = 0,
    GF_SANDBOX_CHROOT,
    GF_SANDBOX_NONE
} gf_sandbox_level;

typedef struct gf_sandbox_paths {
    /* host paths prepared by the caller (build orchestrator) */
    char *root;        /* build root directory (the sandbox filesystem) */
    char *toolchain;   /* mounted at /toolchain (read-only) */
    char *src;         /* mounted at /src (read-write) */
    char *staging;     /* mounted at /staging (read-write) */
    char *deps;        /* mounted at /deps (read-only, may be absent) */
    char *build;       /* mounted at /build (read-write, scratch) */
    /* Host directories bind-mounted READ-ONLY at their standard paths
     * inside the sandbox (bootstrap toolchain trust boundary). NULL-terminated
     * list; missing entries are skipped. NEVER include /home, /etc, /root,
     * /var, /tmp here — those stay private to the sandbox. */
    const char *const *system_dirs;
} gf_sandbox_paths;

void gf_sandbox_paths_free(gf_sandbox_paths *p);

typedef struct gf_sandbox_run {
    gf_sandbox_level level;
    bool netns;              /* isolate network (true = no external net) */
    uint64_t timeout_ms;
    const char *cwd_in;      /* path INSIDE the sandbox (e.g. "/src"); NULL = "/" */
    char *const *envp;       /* full environment (built by caller, not inherited) */
    gf_strbuf *out;
    gf_strbuf *err;
    bool seccomp;            /* install the syscall filter before exec */
    int cgroup_memory_mb;    /* <= 0: no memory limit */
    int cgroup_cpu_pct;      /* <= 0: no cpu limit */
} gf_sandbox_run;

/* Probe namespace availability (forks a child that unshares). */
bool gf_sandbox_userns_available(void);

/* Did the last sandboxed run actually apply seccomp? (diagnostics) */
bool gf_sandbox_last_used_seccomp(void);

/* Create the build-root skeleton (dirs + minimal /etc). Returns 0/-1. */
int gf_sandbox_prepare_root(const gf_sandbox_paths *p, const char *prefix,
                            bool with_resolv);

/* Run a command inside the sandbox. Fills *exit_status (negative when the
 * child could not run). Returns 0 when the command executed (regardless of
 * its exit status), -1 on infrastructure failure. */
int gf_sandbox_exec(const gf_sandbox_paths *p, const gf_sandbox_run *cfg,
                    const char *const argv[], int *exit_status,
                    bool *timed_out);

const char *gf_sandbox_level_name(gf_sandbox_level lvl);

#endif /* GF_SANDBOX_H */
