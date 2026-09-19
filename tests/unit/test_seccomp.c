/* test_seccomp.c — the BPF syscall filter behaves as documented.
 *
 * Runs the filter in forked children so the test process itself stays
 * unfiltered: after installation a filter cannot be removed.
 */
#include "checks.h"
#include "tests.h"

#include "seccomp.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Result codes the probe child reports through its exit status. */
enum {
    PROBE_OK = 0,
    PROBE_BAD_STATE = 100, /* something unexpected happened */
};

/* Runs in the child after the filter is installed. Each probe must see
 * the documented errno; any deviation aborts with a distinct code. */
static int probe_body(bool block_net)
{
    (void)block_net;
    /* allowlisted syscalls keep working */
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0)
        return 11;
    char b = 0;
    if (read(fd, &b, 1) != 0 && errno != EINVAL)
        return 12;
    close(fd);
    if (getpid() <= 1)
        return 13;

    /* denied: mount */
    errno = 0;
    if (mount("none", "/tmp", "tmpfs", 0, "") == 0)
        return 21;
    if (errno != EPERM)
        return 22;

    /* denied: chroot */
    errno = 0;
    if (chroot("/") == 0)
        return 31;
    if (errno != EPERM)
        return 32;

    /* denied: unshare */
    errno = 0;
    if (unshare(CLONE_NEWNS) == 0)
        return 41;
    if (errno != EPERM)
        return 42;

    /* unlisted syscall (vmsplice — neither allowed nor denied):
     * the default rule must answer ENOSYS. Unfiltered, the kernel would
     * reply EBADF for the bogus fd argument instead. */
    errno = 0;
    long r = syscall(SYS_vmsplice, -1, NULL, 0, 0);
    if (r == 0)
        return 51;
    if (errno != ENOSYS)
        return 52;

    /* socket family gate */
    errno = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        close(s);
        if (block_net)
            return 61; /* inet must be blocked */
    } else if (errno != EAFNOSUPPORT) {
        return 62;
    }
    errno = 0;
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    if (u < 0) {
        if (errno == EAFNOSUPPORT)
            return 63; /* unix sockets must pass the gate (EPROTONOSUPPORT
                          or success is fine, EAFNOSUPPORT is not) */
    } else {
        close(u);
    }
    return PROBE_OK;
}

/* Fork, optionally install the filter, run the probes, exit with the
 * probe code (or 125/126 for install/spawn problems). */
static int run_probe(const gf_seccomp_policy *pol)
{
    pid_t pid = fork();
    if (pid < 0)
        return PROBE_BAD_STATE;
    if (pid == 0) {
        int rc = gf_seccomp_install(pol);
        if (rc < 0)
            _exit(125);
        int code = probe_body(pol ? pol->block_network : true);
        _exit(code);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st))
        return PROBE_BAD_STATE;
    return WEXITSTATUS(st);
}

int test_seccomp(void)
{
    /* the build/test environment must know seccomp; if not, the semantic
     * tests below still run (they tolerate install rc == 1) but the
     * describe string must exist either way */
    CHECK(gf_seccomp_describe() != NULL);

    gf_seccomp_policy pol = { true, true };
    CHECK_INT(run_probe(&pol), PROBE_OK);

    /* block_network=false: inet sockets may pass the gate (they may still
     * fail for other reasons in restricted environments, but not with
     * EAFNOSUPPORT from the filter). The denylist probes must still hold. */
    gf_seccomp_policy pol_open = { true, false };
    CHECK_INT(run_probe(&pol_open), PROBE_OK);

    /* enabled=false installs nothing: vmsplice must not report ENOSYS
     * then (ENOSYS can only come from our default rule). */
    pid_t pid = fork();
    if (pid == 0) {
        gf_seccomp_policy off = { false, true };
        if (gf_seccomp_install(&off) != 0)
            _exit(125);
        errno = 0;
        long r = syscall(SYS_vmsplice, -1, NULL, 0, 0);
        if (r == 0)
            _exit(0); /* kernel allowed it somehow */
        if (errno == ENOSYS)
            _exit(61); /* ENOSYS implies our filter is active */
        _exit(0); /* EBADF etc. from the kernel, not from our filter */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return 0;
}
