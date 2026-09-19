/* common.h — shared utilities: memory, strings, paths, fs, exec, logging.
 *
 * Conventions used throughout gitfull:
 *  - Functions returning int use 0 for success and -1 for failure (unless
 *    documented otherwise); failures are logged where they occur.
 *  - Pointer-returning allocation helpers abort on out-of-memory. This is a
 *    deliberate policy for a package manager (same approach as dpkg): OOM is
 *    not recoverable, and aborting keeps every caller short and auditable.
 *  - No system()/popen() anywhere. Child processes are spawned with
 *    fork()+execve() and argv/envp arrays (see exec section below).
 */
#ifndef GF_COMMON_H
#define GF_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---------------------------------------------------------------- exit codes */
#define GF_EXIT_OK        0  /* success */
#define GF_EXIT_ERROR     1  /* general error */
#define GF_EXIT_USAGE     2  /* bad usage */
#define GF_EXIT_CONFLICT  3  /* file conflict / dependency conflict */
#define GF_EXIT_NETWORK   4  /* network failure */
#define GF_EXIT_BUILD     5  /* build failure */
#define GF_EXIT_TEST      6  /* test failure */
#define GF_EXIT_XACT      7  /* transaction failure */
#define GF_EXIT_NOTFOUND  8  /* package not found */

/* ------------------------------------------------------------------- memory */
void *gf_malloc(size_t n);
void *gf_calloc(size_t n, size_t size);
void *gf_realloc(void *p, size_t n);
char *gf_strdup(const char *s);
char *gf_strndup(const char *s, size_t n);
void *gf_memdup(const void *p, size_t n);

/* Frees the pointer and sets it to NULL. Usage: gf_free(p); */
void gf_free_ptr(void *p);

/* -------------------------------------------------------------------- log */
enum gf_log_level {
    GF_LOG_DEBUG = 0,
    GF_LOG_INFO,
    GF_LOG_WARN,
    GF_LOG_ERROR
};

void gf_log_set_level(enum gf_log_level level);
enum gf_log_level gf_log_level(void);
/* Also mirror log output into this file (used by `gitfull logs`). */
void gf_log_set_tee(const char *path);
void gf_log_close_tee(void);

void gf_log(enum gf_log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Log with errno explanation appended ("open /x: No such file or directory"). */
void gf_log_errno(enum gf_log_level lvl, const char *what);

#define LOGD(...) gf_log(GF_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) gf_log(GF_LOG_INFO, __VA_ARGS__)
#define LOGW(...) gf_log(GF_LOG_WARN, __VA_ARGS__)
#define LOGE(...) gf_log(GF_LOG_ERROR, __VA_ARGS__)

/* ----------------------------------------------------------------- strbuf */
/* Growable string buffer. Always NUL-terminated. */
typedef struct gf_strbuf {
    char *s;
    size_t len;
    size_t cap;
} gf_strbuf;

void gf_strbuf_init(gf_strbuf *sb);
void gf_strbuf_free(gf_strbuf *sb);
/* Reset length to 0 (keeps allocation). */
void gf_strbuf_reset(gf_strbuf *sb);
/* Returns the buffer (never NULL; empty buffer returns ""). */
char *gf_strbuf_str(const gf_strbuf *sb);
/* Steal ownership of the C string (buffer reset to empty). */
char *gf_strbuf_steal(gf_strbuf *sb);

int gf_strbuf_append(gf_strbuf *sb, const char *s);
int gf_strbuf_appendn(gf_strbuf *sb, const char *s, size_t n);
int gf_strbuf_appendc(gf_strbuf *sb, char c);
__attribute__((format(printf, 2, 3)))
int gf_strbuf_appendf(gf_strbuf *sb, const char *fmt, ...);
int gf_strbuf_append_hex(gf_strbuf *sb, const uint8_t *bytes, size_t n);

/* ---------------------------------------------------------------- strings */
bool gf_str_eq(const char *a, const char *b);
bool gf_str_ieq(const char *a, const char *b);           /* ASCII case-insens. */
bool gf_str_starts_with(const char *s, const char *prefix);
bool gf_str_ends_with(const char *s, const char *suffix);
/* Trim leading+trailing whitespace in place; returns s. */
char *gf_str_trim(char *s);
/* Natural ordering comparison (digit runs compare numerically). */
int gf_str_naturalcmp(const char *a, const char *b);
/* Split on any of the chars in 'delims'; no empty parts are produced.
 * Returns array of malloc'd strings; *count set. */
char **gf_str_split(const char *s, const char *delims, size_t *count);
void gf_strv_free(char **v, size_t count);
/* Join with separator into a malloc'd string. */
char *gf_strv_join(char *const *v, size_t count, char sep);
/* Percent-encode for use in a URL path/query component. */
char *gf_url_encode(const char *s);
/* POSIX single-quote escaping for safe interpolation into a /bin/sh -c
 * string: 'foo bar' -> '''foo bar'''. malloc'd. */
char *gf_str_shell_quote(const char *s);

/* ------------------------------------------------------------------ paths */
/* Lexical path helpers: no filesystem access; return malloc'd strings unless
 * noted. All security decisions about untrusted paths must go through
 * gf_path_normalize + gf_path_component_ok / gf_path_inside. */
char *gf_path_join(const char *a, const char *b);
/* Join multiple components; argument list must end with NULL. */
char *gf_path_join_multi(const char *first, ...);
/* Normalize lexically: resolve '.', '..' and duplicate slashes.
 * Absolute stays absolute; relative stays relative; ".." past root is
 * dropped (never escapes upward). */
char *gf_path_normalize(const char *path);
bool gf_path_is_abs(const char *path);
/* Lexical containment: is 'path' inside directory 'base' (or equal when
 * allow_equal)? Both must be normalized (leading '/' recommended). */
bool gf_path_inside(const char *base, const char *path, bool allow_equal);
/* Parent directory (malloc'd; NULL for "/"). */
char *gf_path_dirname(const char *path);
/* Final component (malloc'd; "/" yields ""). */
char *gf_path_basename(const char *path);
/* Single-component check: non-empty, no '/', not "." or "..". */
bool gf_path_component_ok(const char *name);

/* ------------------------------------------------------------- filesystem */
bool gf_fs_exists(const char *path);
bool gf_fs_is_dir(const char *path);
bool gf_fs_is_file(const char *path);
bool gf_fs_is_symlink(const char *path);
/* mkdir -p (0755 for newly created dirs). 0 if the dir exists at the end. */
int gf_fs_mkdir_p(const char *path);
/* Same, but silent on failure: for OPTIONAL paths (caches, advisory state);
 * the caller decides whether failure deserves a warning. */
int gf_fs_mkdir_p_soft(const char *path);
/* rmdir if empty; ENOENT is success. */
int gf_fs_rmdir_if_empty(const char *path);
/* Recursive removal that never follows symlinks (openat-based, O_NOFOLLOW).
 * Resists symlink/hardlink substitution races inside the removed tree. */
int gf_fs_rm_rf(const char *path);
/* Read whole file (limit bytes, 0 = unlimited) into a malloc'd buffer;
 * NUL-terminated for convenience. *len (may be NULL) excludes the NUL. */
char *gf_fs_read_file_limit(const char *path, size_t limit, size_t *len);
char *gf_fs_read_file(const char *path, size_t *len);
/* Atomic write: temp file in same dir + fsync + rename + fsync dir. */
int gf_fs_write_file_atomic(const char *path, const void *buf, size_t len);
/* Copy file contents; dst is created with 'mode' (-1 = 0644). */
int gf_fs_copy_file(const char *src, const char *dst, int mode);
/* Copy-on-write clone when the filesystem supports it (FICLONE), full
 * copy otherwise. NEVER shares inodes with the source — used to activate
 * store payloads so live-file modification cannot corrupt the store. */
int gf_fs_reflink_or_copy(const char *src, const char *dst, int mode);
/* fsync a directory (making renames inside it durable). */
int gf_fs_fsync_dir(const char *path);
/* List directory entries (non-recursive) as malloc'd full paths.
 * Skips "." and "..". Empty dir => count 0, out non-NULL. */
int gf_fs_list_dir(const char *path, char ***out, size_t *count);
/* Hardlink src -> dst (fails if dst exists). */
int gf_fs_hardlink(const char *src, const char *dst);
/* Create symlink dst -> target (fails if dst exists). */
int gf_fs_symlink(const char *target, const char *dst);
/* Recursively hardlink a tree (modes preserved, symlinks recreated, never
 * followed). */
int gf_fs_hardlink_tree(const char *src, const char *dst);
/* Recursively copy a tree (modes + mtimes + symlinks preserved, never followed). */
int gf_fs_copy_tree(const char *src, const char *dst);
/* Copy a tree, skipping a top-level ".git" directory. */
int gf_fs_copy_tree_skip_git(const char *src, const char *dst);
/* realpath() wrapper; NULL on error. */
char *gf_fs_realpath(const char *path);
/* Read a symlink's target (malloc'd). */
char *gf_fs_readlink(const char *path);
/* Approximate disk usage (st_blocks * 512) of a path. */
int gf_fs_du(const char *path, uint64_t *out_bytes);

/* ------------------------------------------------------------------ exec */
typedef struct gf_exec_result {
    pid_t pid;         /* child pid (0 when spawn failed) */
    int status;      /* child exit status (valid when !signaled && !timed_out) */
    bool signaled;   /* killed by a signal */
    int signal;      /* signal number when signaled */
    bool timed_out;  /* we killed it due to the timeout */
    int errno_;      /* spawn-side error (fork/exec); 0 when none */
} gf_exec_result;

typedef struct gf_exec_opts {
    const char *cwd;      /* NULL = inherit */
    char *const *envp;    /* NULL = inherit environment (avoid when possible) */
    int stdout_fd;        /* -1 = capture to memory; >=0 = dup2 onto this fd */
    int stderr_fd;
    int stdin_fd;         /* -1 = /dev/null */
    uint64_t timeout_ms;  /* 0 = no timeout */
    gf_strbuf *out;       /* captured stdout (only when stdout_fd == -1) */
    gf_strbuf *err;       /* captured stderr */
    /* Runs in the CHILD after fork, after setsid/signals, before chdir/exec.
     * Used to set up namespaces/mounts. Return != 0 -> child _exit(125). */
    int (*preexec)(void *ud);
    void *preexec_ud;
    /* Runs in the PARENT right after fork (e.g. writing uid/gid maps for a
     * child in a fresh user namespace). Return != 0 -> child is killed and
     * gf_exec fails. */
    int (*parent_hook)(pid_t child, void *ud);
    void *parent_hook_ud;
} gf_exec_opts;

/* Initialize opts with safe defaults (capture everything). */
static inline gf_exec_opts gf_exec_opts_default(void)
{
    gf_exec_opts o = { NULL, NULL, -1, -1, -1, 0, NULL, NULL, NULL, NULL,
                       NULL, NULL };
    return o;
}

/* Execute argv[0] with argv (NULL-terminated). No shell is ever involved.
 * Returns 0 when the child ran to completion (*res filled); -1 for spawn-side
 * errors, which are logged. The child runs in its own process group so a
 * timeout kills the whole tree. */
int gf_exec(const char *const *argv, gf_exec_opts *opts, gf_exec_result *res);

/* Convenience: initialize result; run; report exit via log at DEBUG level. */
int gf_exec_capture(const char *const *argv, gf_exec_opts *opts,
                    gf_exec_result *res);
/* Deep-copy a NULL-terminated argv array into an owned, freeable argv.
 * Every string is duplicated, so the result can be released with
 * gf_strv_free(). */
char **gf_argv_dup(const char *const *argv);
size_t gf_argv_len(const char *const *argv);

/* ------------------------------------------------------------------- time */
/* Current UTC time as ISO-8601 "YYYY-MM-DDTHH:MM:SSZ" (buf >= 21 bytes). */
void gf_time_iso8601_now(char *buf, size_t buflen);
/* Monotonic milliseconds. */
int64_t gf_time_monotonic_ms(void);

/* ------------------------------------------------------------------- misc */
/* Lowercase hex encode; dst needs 2n+1 bytes; returns number encoded. */
size_t gf_hex_encode(char *dst, size_t dstlen, const uint8_t *src, size_t n);
/* Hex decode; -1 on invalid input; dstlen must be >= n/2. */
int gf_hex_decode(uint8_t *dst, size_t dstlen, const char *hex, size_t n);
/* Constant-time comparison (for hashes/secrets). */
int gf_ct_memcmp(const void *a, const void *b, size_t n);
/* mkstemp in 'dir'; returns fd (>=0) and malloc'd path in *path. */
int gf_mkstemp_in(const char *dir, char **path);

#endif /* GF_COMMON_H */
