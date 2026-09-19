/* seccomp.c — classic-BPF seccomp filter, hand-assembled.
 *
 * Instruction layout (x86_64):
 *   [0]  load arch; if != AUDIT_ARCH_X86_64 -> ERRNO(ENOSYS)
 *   [2]  load syscall nr
 *   then, for each rule: JEQ <nr>, 0, 1 + RET <action> pairs.
 *   socket()/socketpair() rules additionally inspect args[0] (the family)
 *   and reload nr afterwards so later rules still match.
 *   The final instruction is the default action: ERRNO(ENOSYS).
 *
 * Every syscall number comes from <sys/syscall.h> (__NR_*), never
 * hand-typed, so the tables track the build host's ABI header.
 */
#include "seccomp.h"

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__x86_64__)

/* --- instruction helpers ------------------------------------------------ */

#define SECCOMP_DATA_NR_OFF   0
#define SECCOMP_DATA_ARCH_OFF 4
#define SECCOMP_DATA_ARG0_OFF 16 /* args[0] low 32 bits (socket family) */

#define BPF_ALLOW_NET   (SECCOMP_RET_ALLOW)
#define BPF_EPERM       (SECCOMP_RET_ERRNO | (uint32_t)EPERM)
#define BPF_ENOSYS      (SECCOMP_RET_ERRNO | (uint32_t)ENOSYS)
#define BPF_EAFNOSUPPORT (SECCOMP_RET_ERRNO | (uint32_t)EAFNOSUPPORT)


/* Syscalls builds may use. Not listed here => ENOSYS (usually harmless:
 * toolchains probe and fall back). Kept deliberately broad on the
 * process/fd/memory/fsignal side because compilers and parallel make
 * exercise all of it. */
static const unsigned long allow_syscalls[] = {
    /* memory */
    __NR_mmap, __NR_munmap, __NR_mprotect, __NR_mremap, __NR_madvise,
    __NR_msync, __NR_brk, __NR_mlock, __NR_munlock,
    /* files + fs */
    __NR_read, __NR_write, __NR_open, __NR_openat, __NR_openat2,
    __NR_close, __NR_close_range, __NR_stat, __NR_fstat, __NR_lstat,
    __NR_newfstatat, __NR_statx, __NR_lseek, __NR_readv, __NR_writev,
    __NR_pread64, __NR_pwrite64, __NR_preadv, __NR_pwritev, __NR_preadv2,
    __NR_pwritev2, __NR_getdents, __NR_getdents64, __NR_fcntl, __NR_dup,
    __NR_dup2, __NR_dup3, __NR_flock, __NR_ioctl, __NR_fsync,
    __NR_fdatasync, __NR_sync, __NR_sync_file_range, __NR_truncate,
    __NR_ftruncate, __NR_fallocate, __NR_fadvise64, __NR_readahead,
    __NR_copy_file_range, __NR_sendfile, __NR_access, __NR_faccessat,
    __NR_faccessat2, __NR_getcwd, __NR_chdir, __NR_fchdir, __NR_rename,
    __NR_renameat, __NR_renameat2, __NR_mkdir, __NR_mkdirat, __NR_rmdir,
    __NR_creat, __NR_link, __NR_linkat, __NR_unlink, __NR_unlinkat,
    __NR_symlink, __NR_symlinkat, __NR_readlink, __NR_readlinkat,
    __NR_chmod, __NR_fchmod, __NR_fchmodat, __NR_chown, __NR_fchown,
    __NR_lchown, __NR_fchownat, __NR_umask, __NR_utimensat, __NR_futimesat,
    __NR_statfs, __NR_fstatfs,
    __NR_inotify_init, __NR_inotify_init1, __NR_inotify_add_watch,
    __NR_inotify_rm_watch, __NR_memfd_create, __NR_syncfs, __NR_mknod,
    __NR_mknodat,
    /* xattrs (cp -a, tar) */
    __NR_setxattr, __NR_lsetxattr, __NR_fsetxattr, __NR_getxattr,
    __NR_lgetxattr, __NR_fgetxattr, __NR_listxattr, __NR_llistxattr,
    __NR_flistxattr, __NR_removexattr, __NR_lremovexattr, __NR_fremovexattr,
    /* signals */
    __NR_rt_sigaction, __NR_rt_sigprocmask, __NR_rt_sigreturn,
    __NR_rt_sigpending, __NR_rt_sigtimedwait, __NR_rt_sigsuspend,
    __NR_sigaltstack, __NR_signalfd4, __NR_restart_syscall,
    /* process */
    __NR_fork, __NR_vfork, __NR_clone, __NR_clone3, __NR_execve,
    __NR_execveat, __NR_exit, __NR_exit_group, __NR_wait4, __NR_waitid,
    __NR_getpid, __NR_getppid, __NR_gettid, __NR_getpgrp, __NR_getpgid,
    __NR_setpgid, __NR_setsid, __NR_getsid, __NR_kill, __NR_tgkill,
    __NR_tkill, __NR_pidfd_open, __NR_pidfd_send_signal, __NR_prctl,
    __NR_arch_prctl, __NR_set_tid_address, __NR_set_robust_list,
    __NR_get_robust_list, __NR_rseq, __NR_capget, __NR_capset,
    /* uid/gid queries (identity, not mutation) */
    __NR_getuid, __NR_geteuid, __NR_getgid, __NR_getegid, __NR_getgroups,
    __NR_getresuid, __NR_getresgid,
    /* scheduling + limits */
    __NR_sched_yield, __NR_sched_getparam, __NR_sched_setparam,
    __NR_sched_getscheduler, __NR_sched_setscheduler,
    __NR_sched_get_priority_max, __NR_sched_get_priority_min,
    __NR_sched_getaffinity, __NR_sched_setaffinity,
    __NR_getpriority, __NR_setpriority, __NR_getrlimit, __NR_setrlimit,
    __NR_prlimit64, __NR_getrusage, __NR_getcpu, __NR_ioprio_get,
    __NR_ioprio_set,
    /* time */
    __NR_gettimeofday, __NR_time, __NR_times, __NR_clock_gettime,
    __NR_clock_getres, __NR_clock_nanosleep, __NR_nanosleep, __NR_getitimer,
    __NR_setitimer, __NR_alarm, __NR_timer_create, __NR_timer_settime,
    __NR_timer_gettime, __NR_timer_getoverrun, __NR_timer_delete,
    __NR_sysinfo, __NR_uname,
    /* pipes/poll/epoll/eventfd */
    __NR_pipe, __NR_pipe2, __NR_poll, __NR_ppoll, __NR_select,
    __NR_pselect6, __NR_epoll_create, __NR_epoll_create1, __NR_epoll_ctl,
    __NR_epoll_wait, __NR_epoll_pwait, __NR_epoll_pwait2, __NR_eventfd,
    __NR_eventfd2, __NR_timerfd_create, __NR_timerfd_settime,
    __NR_timerfd_gettime, __NR_signalfd, __NR_signalfd4,
    /* futex + misc runtime */
    __NR_futex, __NR_getrandom, __NR_membarrier,
    /* AF_UNIX sockets only: creation is gated by the family rule below.
     * With no inet sockets possible, the rest of the socket API is inert. */
    __NR_socket, __NR_socketpair, __NR_connect, __NR_accept, __NR_accept4,
    __NR_bind, __NR_listen, __NR_shutdown, __NR_getsockname,
    __NR_getpeername, __NR_setsockopt, __NR_getsockopt, __NR_sendto,
    __NR_recvfrom, __NR_sendmsg, __NR_recvmsg, __NR_recvmmsg,
    __NR_sendmmsg, __NR_semctl, __NR_semget, __NR_semop,
    __NR_semtimedop, __NR_shmdt, __NR_msgget, __NR_msgctl, __NR_msgrcv,
    __NR_msgsnd,
};

/* Privilege / escape vectors: hard EPERM regardless of anything else. */
static const unsigned long deny_syscalls[] = {
    __NR_ptrace, __NR_mount, __NR_umount2, __NR_pivot_root, __NR_chroot,
    __NR_reboot, __NR_setuid, __NR_setgid, __NR_setreuid, __NR_setregid,
    __NR_setresuid, __NR_setresgid, __NR_setgroups, __NR_setfsuid,
    __NR_setfsgid, __NR_sethostname, __NR_setdomainname, __NR_iopl,
    __NR_ioperm, __NR_vhangup, __NR_modify_ldt, __NR_personality,
    __NR_settimeofday, __NR_adjtimex, __NR_clock_settime, __NR_acct,
    __NR_swapon, __NR_swapoff, __NR_init_module, __NR_finit_module,
    __NR_delete_module, __NR_quotactl, __NR_setns,
    __NR_unshare, __NR_bpf, __NR_perf_event_open, __NR_keyctl,
    __NR_add_key, __NR_request_key, __NR_lookup_dcookie, __NR_uselib,
    __NR_open_by_handle_at, __NR_name_to_handle_at, __NR_fanotify_init,
    __NR_fanotify_mark, __NR_process_vm_readv, __NR_process_vm_writev,
    __NR_userfaultfd, __NR_kcmp, __NR_kexec_load, __NR_kexec_file_load,
    __NR_seccomp,
#ifdef __NR_create_module
    __NR_create_module,
#endif
#ifdef __NR_get_kernel_syms
    __NR_get_kernel_syms,
#endif
#ifdef __NR_query_module
    __NR_query_module,
#endif
#ifdef __NR__sysctl
    __NR__sysctl,
#endif
#ifdef __NR_vm86old
    __NR_vm86old,
#endif
#ifdef __NR_vm86
    __NR_vm86,
#endif
#ifdef __NR_tuxcall
    __NR_tuxcall,
#endif
#ifdef __NR_security
    __NR_security,
#endif
};

/* --- filter assembly ----------------------------------------------------- */

struct prog {
    struct sock_filter insns[1024];
    size_t n;
    bool overflow;
};

static void emit(struct prog *p, uint16_t code, uint8_t jt, uint8_t jf,
                 uint32_t k)
{
    if (p->n >= 1024) {
        if (!p->overflow)
            gf_log(GF_LOG_ERROR, "seccomp: filter overflow (too many rules)");
        p->overflow = true;
        return;
    }
    p->insns[p->n].code = code;
    p->insns[p->n].jt = jt;
    p->insns[p->n].jf = jf;
    p->insns[p->n].k = k;
    p->n++;
}

static void emit_rule(struct prog *p, unsigned long nr, uint32_t action)
{
    emit(p, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, (uint32_t)nr);
    emit(p, BPF_RET | BPF_K, 0, 0, action);
}

/* socket-family gate: allow AF_UNIX, deny other families when
 * block_network is set; otherwise allow any family. */
static void emit_socket_gate(struct prog *p, unsigned long nr,
                             bool block_network)
{
    if (!block_network) {
        emit_rule(p, nr, BPF_ALLOW_NET);
        return;
    }
    /* layout (6 instructions):
     *  [i]   JEQ nr? fall through : skip 5 (-> i+6, after the gate)
     *  [i+1] LD  args[0] (family)
     *  [i+2] JEQ AF_UNIX? fall through : skip 1 (-> i+4)
     *  [i+3] RET allow
     *  [i+4] RET EAFNOSUPPORT
     *  [i+5] LD  nr (reload for subsequent rules) */
    emit(p, BPF_JMP | BPF_JEQ | BPF_K, 0, 5, (uint32_t)nr);
    emit(p, BPF_LD | BPF_W | BPF_ABS, 0, 0, SECCOMP_DATA_ARG0_OFF);
    emit(p, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, (uint32_t)AF_UNIX);
    emit(p, BPF_RET | BPF_K, 0, 0, BPF_ALLOW_NET);
    emit(p, BPF_RET | BPF_K, 0, 0, BPF_EAFNOSUPPORT);
    emit(p, BPF_LD | BPF_W | BPF_ABS, 0, 0, SECCOMP_DATA_NR_OFF);
}

bool gf_seccomp_supported(void)
{
#if defined(PR_GET_SECCOMP)
    /* EINVAL => kernel has seccomp but the prctl is unsupported... in
     * practice PR_GET_SECCOMP returns 0 (disabled) or 2 (filter mode).
     * Anything but -EINVAL means the kernel knows seccomp. */
    errno = 0;
    long r = prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL);
    if (r < 0 && errno == EINVAL)
        return false;
    return true;
#else
    return false;
#endif
}

const char *gf_seccomp_describe(void)
{
    if (!gf_seccomp_supported())
        return "unavailable (kernel without seccomp)";
    return "available (allowlist + denylist, default ENOSYS)";
}

int gf_seccomp_install(const gf_seccomp_policy *pol)
{
    gf_seccomp_policy defpol = { true, true };
    if (!pol)
        pol = &defpol;
    if (!pol->enabled)
        return 0;
    if (!gf_seccomp_supported()) {
        gf_log(GF_LOG_WARN,
               "seccomp: kernel without seccomp support; builds continue "
               "without syscall filtering");
        return 1;
    }

    struct prog p;
    memset(&p, 0, sizeof(p));

    /* arch gate */
    emit(&p, BPF_LD | BPF_W | BPF_ABS, 0, 0, SECCOMP_DATA_ARCH_OFF);
    emit(&p, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, (uint32_t)AUDIT_ARCH_X86_64);
    emit(&p, BPF_RET | BPF_K, 0, 0, BPF_ENOSYS);
    /* load syscall nr */
    emit(&p, BPF_LD | BPF_W | BPF_ABS, 0, 0, SECCOMP_DATA_NR_OFF);

    /* deny list first: hard EPERM even if the allow list would match */
    for (size_t i = 0; i < sizeof(deny_syscalls) / sizeof(deny_syscalls[0]);
         i++)
        emit_rule(&p, deny_syscalls[i], BPF_EPERM);

    /* socket family gates */
    emit_socket_gate(&p, __NR_socket, pol->block_network);
    emit_socket_gate(&p, __NR_socketpair, pol->block_network);

    /* allow list */
    for (size_t i = 0; i < sizeof(allow_syscalls) / sizeof(allow_syscalls[0]);
         i++)
        emit_rule(&p, allow_syscalls[i], SECCOMP_RET_ALLOW);

    /* default: ENOSYS */
    emit(&p, BPF_RET | BPF_K, 0, 0, BPF_ENOSYS);

    if (p.overflow) {
        gf_log(GF_LOG_ERROR,
               "seccomp: rule table exceeded the filter budget; refusing "
               "to install an incomplete filter");
        return -1;
    }

    struct sock_fprog prog = { .len = (unsigned short)p.n,
                               .filter = p.insns };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
        gf_log_errno(GF_LOG_ERROR, "seccomp: PR_SET_NO_NEW_PRIVS");
        return -1;
    }
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0UL, 0UL) != 0) {
        gf_log_errno(GF_LOG_ERROR, "seccomp: PR_SET_SECCOMP (filter)");
        return -1;
    }
    LOGD("seccomp: filter installed (%zu rules)", p.n);
    return 0;
}

#else /* !__x86_64__ */

bool gf_seccomp_supported(void)
{
    return false;
}

const char *gf_seccomp_describe(void)
{
    return "unavailable (unsupported architecture)";
}

int gf_seccomp_install(const gf_seccomp_policy *pol)
{
    (void)pol;
    gf_log(GF_LOG_WARN,
           "seccomp: no filter table for this architecture; builds continue "
           "without syscall filtering");
    return 1;
}

#endif /* __x86_64__ */
