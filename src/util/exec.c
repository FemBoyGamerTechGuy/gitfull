/* exec.c — child process execution with fork+exec, argv/envp arrays, capture,
 * timeouts and process-group kill. system()/popen() are never used. */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void close_safely(int fd)
{
    if (fd >= 0)
        close(fd);
}

int gf_exec(const char *const *argv, gf_exec_opts *opts, gf_exec_result *res)
{
    memset(res, 0, sizeof(*res));

    if (!argv || !argv[0]) {
        gf_log(GF_LOG_ERROR, "exec: empty argv");
        return -1;
    }

    gf_strbuf out_local, err_local;
    gf_strbuf_init(&out_local);
    gf_strbuf_init(&err_local);
    gf_exec_opts defaults = gf_exec_opts_default();
    if (!opts)
        opts = &defaults;

    int outpipe[2] = {-1, -1}, errpipe[2] = {-1, -1}, inpipe[2] = {-1, -1};
    bool capture_out = opts->stdout_fd < 0;
    bool capture_err = opts->stderr_fd < 0;
    bool devnull_in = opts->stdin_fd < 0;

    if (capture_out && pipe2(outpipe, O_CLOEXEC) != 0) {
        gf_log_errno(GF_LOG_ERROR, "pipe");
        return -1;
    }
    if (capture_err && pipe2(errpipe, O_CLOEXEC) != 0) {
        gf_log_errno(GF_LOG_ERROR, "pipe");
        close_safely(outpipe[0]); close_safely(outpipe[1]);
        return -1;
    }
    if (devnull_in && pipe2(inpipe, O_CLOEXEC) != 0) {
        gf_log_errno(GF_LOG_ERROR, "pipe");
        close_safely(outpipe[0]); close_safely(outpipe[1]);
        close_safely(errpipe[0]); close_safely(errpipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        gf_log_errno(GF_LOG_ERROR, "fork");
        close_safely(outpipe[0]); close_safely(outpipe[1]);
        close_safely(errpipe[0]); close_safely(errpipe[1]);
        close_safely(inpipe[0]); close_safely(inpipe[1]);
        return -1;
    }
    res->pid = pid;

    if (pid == 0) {
        /* ---- child ---- */
        setsid(); /* own process group + session (no controlling tty) */
        /* reset signal dispositions */
        for (int sig = 1; sig < NSIG; sig++)
            signal(sig, SIG_DFL);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);

        if (opts->preexec) {
            if (opts->preexec(opts->preexec_ud) != 0)
                _exit(125);
        }

        if (opts->cwd && chdir(opts->cwd) != 0) {
            fprintf(stderr, "gitfull: child: chdir %s: %s\n", opts->cwd,
                    strerror(errno));
            _exit(127);
        }
        if (capture_out) {
            dup2(outpipe[1], STDOUT_FILENO);
        } else {
            if (dup2(opts->stdout_fd, STDOUT_FILENO) < 0)
                _exit(127);
        }
        if (capture_err) {
            dup2(errpipe[1], STDERR_FILENO);
        } else {
            if (dup2(opts->stderr_fd, STDERR_FILENO) < 0)
                _exit(127);
        }
        if (devnull_in) {
            close(inpipe[1]);
            dup2(inpipe[0], STDIN_FILENO);
        } else {
            if (dup2(opts->stdin_fd, STDIN_FILENO) < 0)
                _exit(127);
        }
        /* close everything else we know about */
        close_safely(outpipe[0]); close_safely(outpipe[1]);
        close_safely(errpipe[0]); close_safely(errpipe[1]);
        close_safely(inpipe[0]); close_safely(inpipe[1]);

        char **margv = gf_argv_dup(argv);
        execvpe(argv[0], margv,
                opts->envp ? (char *const *)opts->envp : environ);
        fprintf(stderr, "gitfull: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    /* ---- parent ---- */
    if (opts->parent_hook) {
        if (opts->parent_hook(pid, opts->parent_hook_ud) != 0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            int wstatus_ignore;
            waitpid(pid, &wstatus_ignore, 0);
            close_safely(outpipe[0]); close_safely(outpipe[1]);
            close_safely(errpipe[0]); close_safely(errpipe[1]);
            close_safely(inpipe[0]); close_safely(inpipe[1]);
            return -1;
        }
    }
    close_safely(outpipe[1]);
    close_safely(errpipe[1]);
    close_safely(inpipe[0]);
    if (devnull_in)
        close(inpipe[1]); /* child sees EOF immediately */

    int64_t deadline = opts->timeout_ms > 0
        ? gf_time_monotonic_ms() + (int64_t)opts->timeout_ms
        : 0;
    bool killed = false;

    for (;;) {
        struct pollfd pfds[2];
        int nfd = 0;
        if (capture_out && outpipe[0] >= 0) {
            pfds[nfd].fd = outpipe[0];
            pfds[nfd].events = POLLIN;
            nfd++;
        }
        if (capture_err && errpipe[0] >= 0) {
            pfds[nfd].fd = errpipe[0];
            pfds[nfd].events = POLLIN;
            nfd++;
        }
        if (nfd == 0)
            break;
        int timeout = -1;
        if (deadline > 0) {
            int64_t left = deadline - gf_time_monotonic_ms();
            if (left <= 0) {
                if (!killed) {
                    kill(-pid, SIGKILL);
                    killed = true;
                    res->timed_out = true;
                }
                timeout = 250; /* drain remaining then exit loop */
                if (res->timed_out && killed) {
                    /* give the pipe a short drain window */
                }
            } else {
                timeout = (int)(left > 1000 ? 1000 : left);
            }
        }
        int rc = poll(pfds, (nfds_t)nfd, timeout);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            gf_log_errno(GF_LOG_ERROR, "poll");
            break;
        }
        if (rc == 0)
            continue;
        for (int i = 0; i < nfd; i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP)) {
                char chunk[65536];
                ssize_t n = read(pfds[i].fd, chunk, sizeof(chunk));
                if (n > 0) {
                    if (pfds[i].fd == outpipe[0])
                        gf_strbuf_appendn(&out_local, chunk, (size_t)n);
                    else
                        gf_strbuf_appendn(&err_local, chunk, (size_t)n);
                } else if (n == 0 || (n < 0 && errno != EINTR)) {
                    close(pfds[i].fd);
                    if (pfds[i].fd == outpipe[0])
                        outpipe[0] = -1;
                    else
                        errpipe[0] = -1;
                }
            }
        }
        if (outpipe[0] < 0 && errpipe[0] < 0)
            break;
        if (res->timed_out && killed) {
            /* after timeout kill: drain for a moment, then stop */
            int64_t start = gf_time_monotonic_ms();
            while (gf_time_monotonic_ms() - start < 200 &&
                   (outpipe[0] >= 0 || errpipe[0] >= 0)) {
                struct pollfd p2[2];
                int m = 0;
                if (outpipe[0] >= 0) {
                    p2[m].fd = outpipe[0];
                    p2[m].events = POLLIN;
                    m++;
                }
                if (errpipe[0] >= 0) {
                    p2[m].fd = errpipe[0];
                    p2[m].events = POLLIN;
                    m++;
                }
                if (m == 0)
                    break;
                if (poll(p2, (nfds_t)m, 100) <= 0)
                    break;
                for (int i = 0; i < m; i++) {
                    if (p2[i].revents & (POLLIN | POLLHUP)) {
                        char chunk[65536];
                        ssize_t n = read(p2[i].fd, chunk, sizeof(chunk));
                        if (n > 0) {
                            if (p2[i].fd == outpipe[0])
                                gf_strbuf_appendn(&out_local, chunk, (size_t)n);
                            else
                                gf_strbuf_appendn(&err_local, chunk, (size_t)n);
                        } else {
                            close(p2[i].fd);
                            if (p2[i].fd == outpipe[0])
                                outpipe[0] = -1;
                            else
                                errpipe[0] = -1;
                        }
                    }
                }
            }
            break;
        }
    }

    /* binary-safe: captured output may contain NUL bytes */
    if (opts->out && out_local.len > 0)
        gf_strbuf_appendn(opts->out, out_local.s, out_local.len);
    if (opts->err && err_local.len > 0)
        gf_strbuf_appendn(opts->err, err_local.s, err_local.len);

    int wstatus = 0;
    for (;;) {
        pid_t w = waitpid(pid, &wstatus, 0);
        if (w == pid)
            break;
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            gf_log_errno(GF_LOG_ERROR, "waitpid");
            close_safely(outpipe[0]);
            close_safely(errpipe[0]);
            gf_strbuf_free(&out_local);
            gf_strbuf_free(&err_local);
            return -1;
        }
    }
    close_safely(outpipe[0]);
    close_safely(errpipe[0]);

    if (res->timed_out) {
        res->signaled = true;
        res->signal = SIGKILL;
    } else if (WIFEXITED(wstatus)) {
        res->status = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        res->signaled = true;
        res->signal = WTERMSIG(wstatus);
    }
    gf_strbuf_free(&out_local);
    gf_strbuf_free(&err_local);
    return 0;
}

int gf_exec_capture(const char *const *argv, gf_exec_opts *opts,
                    gf_exec_result *res)
{
    gf_exec_opts defaults = gf_exec_opts_default();
    if (!opts)
        opts = &defaults;
    gf_strbuf cmd;
    gf_strbuf_init(&cmd);
    for (size_t i = 0; argv && argv[i]; i++) {
        if (i > 0)
            gf_strbuf_appendc(&cmd, ' ');
        gf_strbuf_append(&cmd, argv[i]);
    }
    LOGD("exec: %s", gf_strbuf_str(&cmd));
    gf_strbuf_free(&cmd);
    return gf_exec(argv, opts, res);
}
