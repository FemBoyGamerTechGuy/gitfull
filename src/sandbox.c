/* sandbox.c — namespace + seccomp + cgroup build isolation. */
#include "sandbox.h"

#include "cgroup.h"
#include "common.h"
#include "seccomp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* diagnostics: did the most recent sandboxed run install the seccomp filter? */
static bool g_last_used_seccomp;

bool gf_sandbox_last_used_seccomp(void)
{
    return g_last_used_seccomp;
}

void gf_sandbox_paths_free(gf_sandbox_paths *p)
{
    if (!p)
        return;
    free(p->root);
    free(p->toolchain);
    free(p->src);
    free(p->staging);
    free(p->deps);
    free(p->build);
    memset(p, 0, sizeof(*p));
}

const char *gf_sandbox_level_name(gf_sandbox_level lvl)
{
    switch (lvl) {
    case GF_SANDBOX_USERNS: return "userns";
    case GF_SANDBOX_CHROOT: return "chroot";
    case GF_SANDBOX_NONE:
    default:                return "none";
    }
}

bool gf_sandbox_userns_available(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS |
                    CLONE_NEWIPC;
        if (unshare(flags) != 0)
            _exit(1);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int gf_sandbox_prepare_root(const gf_sandbox_paths *p, const char *prefix,
                            bool with_resolv)
{
    static const char *dirs[] = { "toolchain", "src",   "build",  "staging",
                                  "deps",       "tmp",   "home",   "home/build",
                                  "dev",        "proc",  "etc",    "oldroot",
                                  "var",        "var/tmp", NULL };
    for (int i = 0; dirs[i]; i++) {
        char *d = gf_path_join(p->root, dirs[i]);
        if (gf_fs_mkdir_p(d) != 0) {
            free(d);
            return -1;
        }
        free(d);
    }
    /* minimal /etc: passwd, group, and optional resolv.conf for fetches */
    char *passwd = gf_path_join(p->root, "etc/passwd");
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    gf_strbuf_appendf(&sb,
                      "root:x:0:0:root:/root:/bin/sh\n"
                      "build:x:%lu:%lu:gitfull build user:/home/build:/bin/sh\n",
                      (unsigned long)geteuid(), (unsigned long)getegid());
    if (gf_fs_write_file_atomic(passwd, sb.s, sb.len) != 0) {
        free(passwd);
        gf_strbuf_free(&sb);
        return -1;
    }
    free(passwd);
    gf_strbuf_reset(&sb);
    gf_strbuf_appendf(&sb, "root:x:0:\nbuild:x:%lu:\n",
                      (unsigned long)getegid());
    char *group = gf_path_join(p->root, "etc/group");
    if (gf_fs_write_file_atomic(group, sb.s, sb.len) != 0) {
        free(group);
        gf_strbuf_free(&sb);
        return -1;
    }
    free(group);
    gf_strbuf_free(&sb);
    if (with_resolv) {
        /* copy host resolv.conf so dependency fetches can resolve names */
        char *src = gf_fs_read_file("/etc/resolv.conf", NULL);
        if (src) {
            char *dst = gf_path_join(p->root, "etc/resolv.conf");
            gf_fs_write_file_atomic(dst, src, strlen(src));
            free(dst);
            free(src);
        }
    }
    (void)prefix;
    return 0;
}

/* ---- child-side setup ---------------------------------------------------- */

struct sb_ctx {
    gf_sandbox_paths paths;   /* by value: host paths (parent memory) */
    gf_sandbox_level level;
    bool netns;
    const char *cwd_in;
    int sync_wr;              /* child -> parent signal pipe (write end) */
    int sync_rd;              /* parent -> child ack pipe (read end) */
    gf_seccomp_policy seccomp; /* syscall filter policy */
};

static int write_file_str(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    size_t len = strlen(content);
    ssize_t w = write(fd, content, len);
    close(fd);
    if (w != (ssize_t)len)
        return -1;
    return 0;
}

static int bind_mount(const char *src, const char *dst, bool readonly)
{
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0) {
        gf_log_errno(GF_LOG_ERROR, "bind mount");
        return -1;
    }
    if (readonly) {
        if (mount(NULL, dst, NULL, MS_BIND | MS_REC | MS_REMOUNT | MS_RDONLY,
                  NULL) != 0) {
            gf_log_errno(GF_LOG_ERROR, "remount ro");
            return -1;
        }
    }
    return 0;
}

static int setup_userns_maps_for(pid_t child)
{
    /* Runs in the PARENT (still in the initial user namespace): mapping our
     * own uid for the child is allowed without privileges. */
    char path[128], map[64];
    int rc = 0;
    snprintf(path, sizeof(path), "/proc/%d/setgroups", child);
    if (write_file_str(path, "deny\n") != 0) {
        /* old kernels / single-group processes: non-fatal */
    }
    snprintf(path, sizeof(path), "/proc/%d/uid_map", child);
    snprintf(map, sizeof(map), "0 %u 1\n", geteuid());
    if (write_file_str(path, map) != 0) {
        gf_log_errno(GF_LOG_ERROR, path);
        rc = -1;
    }
    snprintf(path, sizeof(path), "/proc/%d/gid_map", child);
    snprintf(map, sizeof(map), "0 %u 1\n", getegid());
    if (write_file_str(path, map) != 0) {
        gf_log_errno(GF_LOG_ERROR, path);
        rc = -1;
    }
    return rc;
}

static int bind_system_dirs(gf_sandbox_paths *p)
{
    if (p->system_dirs) {
        for (size_t i = 0; p->system_dirs[i]; i++) {
            const char *sd = p->system_dirs[i];
            if (!gf_path_is_abs(sd) || !gf_fs_is_dir(sd))
                continue;
            char *base = gf_path_basename(sd);
            if (!gf_path_component_ok(base)) {
                free(base);
                continue;
            }
            char *dst = gf_path_join(p->root, base);
            if (gf_fs_mkdir_p(dst) != 0) {
                free(dst);
                free(base);
                return -1;
            }
            if (bind_mount(sd, dst, true) != 0) {
                free(dst);
                free(base);
                return -1;
            }
            free(dst);
            free(base);
        }
    }
    /* Debian/Ubuntu alternatives: /usr/bin/cc -> /etc/alternatives/cc -> gcc.
     * Without this bind, PATH lookups of cc/c++ break inside the sandbox. */
    if (gf_fs_is_dir("/etc/alternatives")) {
        char *dst = gf_path_join(p->root, "etc/alternatives");
        if (gf_fs_mkdir_p(dst) == 0) {
            if (bind_mount("/etc/alternatives", dst, true) != 0)
                gf_log_errno(GF_LOG_DEBUG, "alternatives bind");
        }
        free(dst);
    }
    return 0;
}

/* Re-exec signal dispositions for status mirroring. */
static void mirror_exit(int st)
{
    if (WIFEXITED(st)) {
        _exit(WEXITSTATUS(st));
    }
    if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        signal(sig, SIG_DFL);
        raise(sig);
    }
    _exit(1);
}

static int child_setup(void *ud)
{
    struct sb_ctx *c = ud;
    gf_sandbox_paths *p = &c->paths;

    if (c->level == GF_SANDBOX_USERNS) {
        int flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID |
                    CLONE_NEWUTS | CLONE_NEWIPC;
        if (c->netns)
            flags |= CLONE_NEWNET;
        if (unshare(flags) != 0) {
            gf_log_errno(GF_LOG_ERROR, "unshare");
            return -1;
        }
        /* signal parent that namespaces exist, then wait for uid/gid maps */
        char sig = 'R';
        if (write(c->sync_wr, &sig, 1) != 1)
            return -1;
        char ack = 0;
        if (read(c->sync_rd, &ack, 1) != 1 || ack != 'A')
            return -1;
        if (getuid() != 0) {
            gf_log(GF_LOG_ERROR, "sandbox: uid mapping failed");
            return -1;
        }
        /* hostname isolation */
        sethostname("gitfull-build", 13);

        /* CLONE_NEWPID only affects CHILDREN: fork so the sandbox command
         * lineage lives inside the pid namespace with a proper init that
         * reaps orphans (this is what `unshare --pid --fork` does). */
        pid_t mid = fork();
        if (mid < 0)
            return -1;
        if (mid > 0) {
            /* intermediate process: mirror the pid-1 init's status */
            int st = 0;
            while (waitpid(mid, &st, 0) < 0 && errno == EINTR)
                ;
            mirror_exit(st);
        }
        /* we are the pid-namespace init candidate; setup continues below and
         * the command itself is forked after pivot_root. */
    } else if (c->level == GF_SANDBOX_NONE) {
        /* degraded: no namespace isolation (explicitly configured).
         * seccomp still applies — it is namespace-independent. */
        if (chdir(c->cwd_in ? c->cwd_in : "/") != 0)
            return -1;
        if (gf_seccomp_install(&c->seccomp) < 0)
            return -1;
        g_last_used_seccomp = c->seccomp.enabled;
        return 0;
    }

    /* make all mounts private so we don't leak mounts back to the host */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 &&
        c->level != GF_SANDBOX_NONE) {
        gf_log_errno(GF_LOG_ERROR, "mount private");
        return -1;
    }

    if (c->level == GF_SANDBOX_USERNS) {
        /* ---- bind the sandbox contents into the build root ---- */
        char *dst;
        /* read-only binds */
        if (p->toolchain && gf_fs_is_dir(p->toolchain)) {
            dst = gf_path_join(p->root, "toolchain");
            if (bind_mount(p->toolchain, dst, true) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->deps && gf_fs_is_dir(p->deps)) {
            dst = gf_path_join(p->root, "deps");
            if (bind_mount(p->deps, dst, true) != 0) { free(dst); return -1; }
            free(dst);
        }
        /* read-write binds */
        if (p->src && gf_fs_is_dir(p->src)) {
            dst = gf_path_join(p->root, "src");
            if (bind_mount(p->src, dst, false) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->build && gf_fs_is_dir(p->build)) {
            dst = gf_path_join(p->root, "build");
            if (bind_mount(p->build, dst, false) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->staging && gf_fs_is_dir(p->staging)) {
            dst = gf_path_join(p->root, "staging");
            if (bind_mount(p->staging, dst, false) != 0) { free(dst); return -1; }
            free(dst);
        }
        /* bootstrap system dirs (read-only host userland) */
        if (bind_system_dirs(p) != 0)
            return -1;

        /* tmpfs for /tmp, /dev, /home. NOTE: /dev must NOT be sticky:
         * a sticky /dev blocks O_CREAT|O_TRUNC on root-owned-but-unmapped
         * device nodes (dash redirects use O_TRUNC). */
        static const char *tmpfsdirs[] = { "tmp", "dev", "home", "var/tmp", NULL };
        for (int i = 0; tmpfsdirs[i]; i++) {
            dst = gf_path_join(p->root, tmpfsdirs[i]);
            char opts[64];
            bool dev = strcmp(tmpfsdirs[i], "dev") == 0;
            snprintf(opts, sizeof(opts), "size=64m,mode=%s,nr_inodes=16k",
                     dev ? "0755" : "1777");
            if (mount("tmpfs", dst, "tmpfs", 0, opts) != 0) {
                gf_log_errno(GF_LOG_ERROR, "tmpfs mount");
                free(dst);
                return -1;
            }
            free(dst);
        }
        /* /home/build was hidden by the /home tmpfs: recreate */
        dst = gf_path_join(p->root, "home/build");
        if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
            gf_log_errno(GF_LOG_ERROR, "mkdir home/build");
            free(dst);
            return -1;
        }
        free(dst);

        /* core device nodes: bind host's (file binds) */
        static const char *devs[] = { "null", "zero", "urandom", "random",
                                      "full", NULL };
        for (int i = 0; devs[i]; i++) {
            char *hd = gf_path_join_multi("/dev", devs[i], NULL);
            dst = gf_path_join_multi(p->root, "dev", devs[i], NULL);
            if (gf_fs_exists(hd)) {
                /* target must exist as a regular file for a file bind */
                int fd = open(dst, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
                if (fd >= 0)
                    close(fd);
                if (mount(hd, dst, NULL, MS_BIND, NULL) != 0) {
                    gf_log_errno(GF_LOG_DEBUG, "dev bind");
                }
            }
            free(hd);
            free(dst);
        }

        /* ---- pivot_root into the build root ---- */
        if (mount(p->root, p->root, NULL, MS_BIND | MS_REC, NULL) != 0) {
            gf_log_errno(GF_LOG_ERROR, "self bind");
            return -1;
        }
        if (chdir(p->root) != 0)
            return -1;
        /* we're uid 0 inside the userns; chroot+pivot needs CAP_SYS_CHROOT
         * which we have in our userns for chroot(2)... use pivot_root. */
        if (syscall(SYS_pivot_root, ".", "oldroot") != 0) {
            gf_log_errno(GF_LOG_ERROR, "pivot_root");
            return -1;
        }
        /* detach the old root (with all host mounts) */
        umount2("/oldroot", MNT_DETACH);
        if (chdir("/") != 0)
            return -1;
        /* /proc of the new pid namespace — mounted after pivot so it is a
         * top-level mount in the sandbox root. Some hardened kernels deny
         * nested procfs; that degrades gracefully (warning only). */
        if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC, NULL) != 0) {
            gf_log_errno(GF_LOG_WARN,
                         "proc mount (sandbox /proc unavailable; builds still run)");
        }
        /* Make the sandbox root itself read-only: only the explicitly
         * writable mounts (/src, /build, /staging, /tmp, /home, /var/tmp,
         * /dev) stay writable. Everything else — /etc, /toolchain, /deps,
         * system dirs — is read-only for the build. */
        if (mount(NULL, "/", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0) {
            gf_log_errno(GF_LOG_WARN, "root read-only remount");
        }
    } else if (c->level == GF_SANDBOX_CHROOT) {
        /* real root: classic chroot after mounts (same binds) */
        char *dst;
        if (p->toolchain && gf_fs_is_dir(p->toolchain)) {
            dst = gf_path_join(p->root, "toolchain");
            if (bind_mount(p->toolchain, dst, true) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->deps && gf_fs_is_dir(p->deps)) {
            dst = gf_path_join(p->root, "deps");
            if (bind_mount(p->deps, dst, true) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->src && gf_fs_is_dir(p->src)) {
            dst = gf_path_join(p->root, "src");
            if (bind_mount(p->src, dst, false) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->build && gf_fs_is_dir(p->build)) {
            dst = gf_path_join(p->root, "build");
            if (bind_mount(p->build, dst, false) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (p->staging && gf_fs_is_dir(p->staging)) {
            dst = gf_path_join(p->root, "staging");
            if (bind_mount(p->staging, dst, false) != 0) { free(dst); return -1; }
            free(dst);
        }
        if (bind_system_dirs(p) != 0)
            return -1;
        static const char *tmpfsdirs[] = { "tmp", "home", NULL };
        for (int i = 0; tmpfsdirs[i]; i++) {
            dst = gf_path_join(p->root, tmpfsdirs[i]);
            if (mount("tmpfs", dst, "tmpfs", 0, "size=64m,mode=1777") != 0) {
                gf_log_errno(GF_LOG_DEBUG, "tmpfs");
            }
            free(dst);
        }
        dst = gf_path_join(p->root, "proc");
        if (mount("proc", dst, "proc", MS_NOSUID | MS_NOEXEC, NULL) != 0) {
            gf_log_errno(GF_LOG_DEBUG, "proc");
        }
        free(dst);
        if (chroot(p->root) != 0) {
            gf_log_errno(GF_LOG_ERROR, "chroot");
            return -1;
        }
        if (chdir("/") != 0)
            return -1;
    }

    /* userns level: we are PID 1 of the new pid namespace. Install the
     * seccomp filter BEFORE forking the command so it is inherited by the
     * build and every process it spawns (filters cannot be removed).
     * PID 1's own loop only uses waitpid/unlink/rename — all allowed.
     * When PID 1 exits, the kernel kills every remaining process in the
     * namespace. */
    if (gf_seccomp_install(&c->seccomp) < 0)
        return -1;
    g_last_used_seccomp = c->seccomp.enabled;
    if (c->level == GF_SANDBOX_USERNS) {
        pid_t cmd = fork();
        if (cmd < 0)
            return -1;
        if (cmd > 0) {
            for (;;) {
                int st = 0;
                pid_t w = waitpid(-1, &st, 0);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    _exit(1);
                }
                if (w == cmd) {
                    mirror_exit(st);
                }
                /* some orphan: keep reaping */
            }
        }
    }

    if (c->cwd_in && chdir(c->cwd_in) != 0) {
        gf_log_errno(GF_LOG_ERROR, c->cwd_in);
        return -1;
    }
    return 0;
}

struct parent_map_ctx {
    int sig_rd;    /* read end of child->parent pipe */
    int ack_wr;    /* write end of parent->child pipe */
    bool userns;   /* write maps only for the userns level */
    int cg_mem;    /* cgroup memory limit (MB); <= 0 = none */
    int cg_cpu;    /* cgroup cpu limit (percent); <= 0 = none */
};

static int parent_map_hook(pid_t child, void *ud)
{
    struct parent_map_ctx *pm = ud;
    if (pm->userns) {
        char sig = 0;
        if (read(pm->sig_rd, &sig, 1) != 1 || sig != 'R') {
            gf_log(GF_LOG_ERROR, "sandbox: child namespace handshake failed");
            return -1;
        }
        if (setup_userns_maps_for(child) != 0)
            return -1;
    }
    /* apply resource limits while the child is quiescent: in userns mode
     * it is blocked reading the ack byte; in other modes it has only just
     * been forked. Grandchildren forked later inherit the membership. */
    if (pm->cg_mem > 0 || pm->cg_cpu > 0) {
        char cgid[32];
        snprintf(cgid, sizeof(cgid), "b-%d", (int)child);
        (void)gf_cgroup_apply(child, cgid, pm->cg_mem, pm->cg_cpu);
    }
    if (pm->userns) {
        char ack = 'A';
        if (write(pm->ack_wr, &ack, 1) != 1)
            return -1;
    }
    return 0;
}

int gf_sandbox_exec(const gf_sandbox_paths *p, const gf_sandbox_run *cfg,
                    const char *const argv[], int *exit_status,
                    bool *timed_out)
{
    *exit_status = -1;
    if (timed_out)
        *timed_out = false;

    struct sb_ctx c;
    memset(&c, 0, sizeof(c));
    c.paths = *p;                 /* host paths by value */
    c.level = cfg->level;
    c.netns = cfg->netns;
    c.cwd_in = cfg->cwd_in;
    c.seccomp.enabled = cfg->seccomp;
    c.seccomp.block_network = cfg->netns;
    g_last_used_seccomp = false;

    int sig_pipe[2] = { -1, -1 };  /* child -> parent */
    int ack_pipe[2] = { -1, -1 };  /* parent -> child */
    struct parent_map_ctx pm = { -1, -1, cfg->level == GF_SANDBOX_USERNS,
                                 cfg->cgroup_memory_mb, cfg->cgroup_cpu_pct };
    if (cfg->level == GF_SANDBOX_USERNS) {
        if (pipe2(sig_pipe, O_CLOEXEC) != 0 || pipe2(ack_pipe, O_CLOEXEC) != 0) {
            gf_log_errno(GF_LOG_ERROR, "pipe");
            return -1;
        }
        c.sync_wr = sig_pipe[1];
        c.sync_rd = ack_pipe[0];
        pm.sig_rd = sig_pipe[0];
        pm.ack_wr = ack_pipe[1];
    }

    gf_exec_opts eo = gf_exec_opts_default();
    eo.preexec = child_setup;
    eo.preexec_ud = &c;
    eo.parent_hook = parent_map_hook;
    eo.parent_hook_ud = &pm;
    eo.envp = cfg->envp;
    eo.out = cfg->out;
    eo.err = cfg->err;
    eo.timeout_ms = cfg->timeout_ms;

    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    LOGD("sandbox: %s", argv[0]);
    int rc = gf_exec_capture(argv, &eo, &res);
    for (int i = 0; i < 2; i++) {
        if (sig_pipe[i] >= 0)
            close(sig_pipe[i]);
        if (ack_pipe[i] >= 0)
            close(ack_pipe[i]);
    }
    /* release the cgroup for this run (children have exited by now) */
    if (cfg->cgroup_memory_mb > 0 || cfg->cgroup_cpu_pct > 0) {
        char cgid[32];
        snprintf(cgid, sizeof(cgid), "b-%d", (int)res.pid);
        gf_cgroup_release(cgid);
    }
    if (rc != 0)
        return -1;
    if (res.timed_out && timed_out)
        *timed_out = true;
    if (res.signaled)
        *exit_status = -res.signal;
    else if (res.status == 125)
        return -1; /* child setup failed */
    else
        *exit_status = res.status;
    return 0;
}
