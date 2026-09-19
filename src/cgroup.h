/* cgroup.h — best-effort resource limits for sandboxed builds (cgroup v2).
 *
 * gitfull applies memory.max / cpu.max limits to each build via a
 * delegated cgroup v2 hierarchy when the host provides one:
 *   /sys/fs/cgroup/gitfull/<transaction-id>/...
 *
 * On hosts where the unified hierarchy is not delegated to the invoking
 * user (containers, most shared systems) the limits are skipped with a
 * one-line notice — namespaces and seccomp still apply. Limits are
 * therefore hardening, not a correctness requirement.
 */
#ifndef GF_CGROUP_H
#define GF_CGROUP_H

#include <stdbool.h>
#include <sys/types.h>

/* Is a (writable) cgroup v2 unified hierarchy mounted? */
bool gf_cgroup_available(void);

/* Create /sys/fs/cgroup/gitfull/<id> with limits and move pid into it.
 * memory_mb <= 0 or cpu_percent <= 0 skips that limit. Best effort:
 * returns 0 when limits were applied OR cleanly skipped (skip is logged
 * at debug level); -1 only when a limit was partially applied and the
 * caller should treat containment as compromised. */
int gf_cgroup_apply(pid_t pid, const char *id, int memory_mb,
                    int cpu_percent);

/* Remove the cgroup created for <id> (after the build exited). Best effort. */
void gf_cgroup_release(const char *id);

#endif /* GF_CGROUP_H */
