/* cgroup.c — best-effort cgroup v2 resource limits for builds. */
#include "cgroup.h"

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GF_CGROUP_ROOT "/sys/fs/cgroup"
#define GF_CGROUP_BASE GF_CGROUP_ROOT "/gitfull"

static int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t len = strlen(content);
    ssize_t w = write(fd, content, len);
    int saved = errno;
    close(fd);
    errno = saved;
    return (w == (ssize_t)len) ? 0 : -1;
}

bool gf_cgroup_available(void)
{
    /* v2 marker: the unified hierarchy exposes "cgroup.controllers" */
    return gf_fs_is_file(GF_CGROUP_ROOT "/cgroup.controllers");
}

int gf_cgroup_apply(pid_t pid, const char *id, int memory_mb, int cpu_percent)
{
    if (!gf_cgroup_available()) {
        LOGD("cgroup: no v2 unified hierarchy; resource limits skipped");
        return 0;
    }
    if (memory_mb <= 0 && cpu_percent <= 0)
        return 0; /* limits not configured */
    if (!id || !*id || !gf_path_component_ok(id))
        return 0;

    /* the parent "gitfull" group must exist (admin or first use creates it) */
    if (!gf_fs_is_dir(GF_CGROUP_BASE)) {
        if (mkdir(GF_CGROUP_BASE, 0755) != 0 && errno != EEXIST) {
            LOGD("cgroup: cannot create %s (%s); resource limits skipped",
                 GF_CGROUP_BASE, strerror(errno));
            return 0;
        }
    }
    char *dir = gf_path_join(GF_CGROUP_BASE, id);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        LOGD("cgroup: cannot create %s (%s); resource limits skipped",
             dir, strerror(errno));
        free(dir);
        return 0;
    }

    bool applied_any = false;
    if (memory_mb > 0) {
        char *p = gf_path_join(dir, "memory.max");
        char val[32];
        snprintf(val, sizeof(val), "%dM", memory_mb);
        if (write_file(p, val) == 0) {
            applied_any = true;
        } else {
            LOGD("cgroup: memory.max not writable (%s)", strerror(errno));
        }
        free(p);
    }
    if (cpu_percent > 0) {
        char *p = gf_path_join(dir, "cpu.max");
        char val[48];
        /* percent of one CPU period (100ms): quota = pct * 1000 usec */
        int quota = cpu_percent * 1000;
        if (quota < 1000)
            quota = 1000;
        snprintf(val, sizeof(val), "%d 100000", quota);
        if (write_file(p, val) == 0) {
            applied_any = true;
        } else {
            LOGD("cgroup: cpu.max not writable (%s)", strerror(errno));
        }
        free(p);
    }

    int rc = 0;
    if (applied_any) {
        char *p = gf_path_join(dir, "cgroup.procs");
        char val[32];
        snprintf(val, sizeof(val), "%d", (int)pid);
        if (write_file(p, val) != 0) {
            /* the child may already have exited; limits simply won't bind */
            LOGD("cgroup: moving pid %d failed (%s)", (int)pid,
                 strerror(errno));
        }
        free(p);
    }
    if (!applied_any) {
        /* nothing applied: remove the empty group so we don't leak dirs */
        rmdir(dir);
    }
    free(dir);
    return rc;
}

void gf_cgroup_release(const char *id)
{
    if (!id || !*id || !gf_path_component_ok(id))
        return;
    if (!gf_fs_is_dir(GF_CGROUP_BASE))
        return;
    char *dir = gf_path_join(GF_CGROUP_BASE, id);
    /* only succeeds when empty (processes gone) — exactly what we want */
    if (rmdir(dir) != 0)
        LOGD("cgroup: %s not empty; left in place", dir);
    free(dir);
}
