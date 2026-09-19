/* fs.c — filesystem helpers. Security-relevant operations (rm_rf, copy,
 * hardlink) are openat/O_NOFOLLOW based to resist symlink races. */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

bool gf_fs_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

bool gf_fs_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool gf_fs_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool gf_fs_is_symlink(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static int mkdir_p_impl(const char *path, bool quiet)
{
    if (!path || !*path)
        return -1;
    size_t len = strlen(path);
    char *copy = gf_strdup(path);
    /* strip trailing slashes */
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                if (!quiet)
                    gf_log_errno(GF_LOG_ERROR, "mkdir");
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        if (!quiet)
            gf_log_errno(GF_LOG_ERROR, "mkdir");
        free(copy);
        return -1;
    }
    free(copy);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (!quiet)
            gf_log(GF_LOG_ERROR, "not a directory: %s", path);
        return -1;
    }
    return 0;
}

int gf_fs_mkdir_p(const char *path)
{
    return mkdir_p_impl(path, false);
}

int gf_fs_mkdir_p_soft(const char *path)
{
    /* Optional-path variant: failure is logged as a warning by the CALLER
     * (or ignored); used for caches and advisory state. */
    return mkdir_p_impl(path, true);
}

int gf_fs_rmdir_if_empty(const char *path)
{
    if (rmdir(path) != 0 && errno != ENOENT) {
        gf_log_errno(GF_LOG_DEBUG, path);
        return -1;
    }
    return 0;
}

/* Recursive removal relative to a directory fd, never following symlinks. */
static int rm_rf_at(int dirfd, const char *name)
{
    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        /* not a directory (or vanished): plain unlink */
        if (unlinkat(dirfd, name, 0) != 0 && errno != ENOENT) {
            gf_log_errno(GF_LOG_ERROR, "unlink");
            return -1;
        }
        return 0;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        gf_log_errno(GF_LOG_ERROR, "fdopendir");
        close(fd);
        return -1;
    }
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (rm_rf_at(fd, de->d_name) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d); /* closes fd */
    if (rc == 0 && unlinkat(dirfd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        gf_log_errno(GF_LOG_ERROR, "rmdir");
        rc = -1;
    }
    return rc;
}

int gf_fs_rm_rf(const char *path)
{
    if (!gf_fs_exists(path))
        return 0;
    /* Remove the top-level name relative to its parent to keep the openat
     * discipline all the way up. */
    char *parent = gf_path_dirname(path);
    char *base = gf_path_basename(path);
    if (!gf_path_component_ok(base)) {
        gf_log(GF_LOG_ERROR, "refusing to remove unsafe path: %s", path);
        free(parent);
        free(base);
        return -1;
    }
    int pfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pfd < 0) {
        gf_log_errno(GF_LOG_ERROR, parent);
        free(parent);
        free(base);
        return -1;
    }
    int rc = rm_rf_at(pfd, base);
    close(pfd);
    free(parent);
    free(base);
    return rc;
}

char *gf_fs_read_file_limit(const char *path, size_t limit, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        gf_log_errno(GF_LOG_ERROR, path);
        return NULL;
    }
    gf_strbuf sb;
    gf_strbuf_init(&sb);
    char chunk[65536];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            gf_log_errno(GF_LOG_ERROR, "read");
            close(fd);
            gf_strbuf_free(&sb);
            return NULL;
        }
        if (n == 0)
            break;
        if (limit > 0 && sb.len + (size_t)n > limit) {
            gf_log(GF_LOG_ERROR, "file too large: %s (limit %zu)", path, limit);
            close(fd);
            gf_strbuf_free(&sb);
            return NULL;
        }
        gf_strbuf_appendn(&sb, chunk, (size_t)n);
    }
    close(fd);
    if (len)
        *len = sb.len;
    return gf_strbuf_steal(&sb);
}

char *gf_fs_read_file(const char *path, size_t *len)
{
    return gf_fs_read_file_limit(path, 0, len);
}

int gf_fs_write_file_atomic(const char *path, const void *buf, size_t len)
{
    char *dir = gf_path_dirname(path);
    char *base = gf_path_basename(path);
    char *tmp = NULL;
    int rc = -1;
    int fd = gf_mkstemp_in(dir, &tmp);
    if (fd < 0)
        goto out;
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            gf_log_errno(GF_LOG_ERROR, "write");
            goto out;
        }
        p += n;
        left -= (size_t)n;
    }
    if (fchmod(fd, 0644) != 0) {
        gf_log_errno(GF_LOG_ERROR, "fchmod");
        goto out;
    }
    if (fsync(fd) != 0 && errno != EINVAL) {
        gf_log_errno(GF_LOG_ERROR, "fsync");
        goto out;
    }
    close(fd);
    fd = -1;
    if (rename(tmp, path) != 0) {
        gf_log_errno(GF_LOG_ERROR, "rename");
        goto out;
    }
    free(tmp);
    tmp = NULL;
    gf_fs_fsync_dir(dir);
    rc = 0;
out:
    if (fd >= 0)
        close(fd);
    if (tmp)
        unlink(tmp);
    free(tmp);
    free(dir);
    free(base);
    return rc;
}

int gf_fs_copy_file(const char *src, const char *dst, int mode)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        gf_log_errno(GF_LOG_ERROR, src);
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                   mode >= 0 ? (mode_t)mode : (mode_t)0644);
    if (out < 0) {
        gf_log_errno(GF_LOG_ERROR, dst);
        close(in);
        return -1;
    }
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            gf_log_errno(GF_LOG_ERROR, "read");
            rc = -1;
            break;
        }
        if (n == 0)
            break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                gf_log_errno(GF_LOG_ERROR, "write");
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
    }
    if (rc == 0 && fsync(out) != 0 && errno != EINVAL)
        rc = -1;
    close(in);
    close(out);
    return rc;
}

int gf_fs_fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    if (rc != 0 && errno == EINVAL) /* some fs don't support dir fsync */
        rc = 0;
    close(fd);
    return rc;
}

int gf_fs_reflink_or_copy(const char *src, const char *dst, int mode)
{
    /* Activate store payload WITHOUT sharing inodes with the immutable
     * store: a reflink (FICLONE) is copy-on-write, so in-place modification
     * of the live file never corrupts the store copy — unlike a hardlink.
     * Filesystems without reflink (or cross-device) fall back to a full
     * copy, which has the same guarantee. */
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        gf_log_errno(GF_LOG_ERROR, src);
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                   mode >= 0 ? (mode_t)mode : (mode_t)0644);
    if (out < 0) {
        gf_log_errno(GF_LOG_ERROR, dst);
        close(in);
        return -1;
    }
    if (ioctl(out, FICLONE, in) == 0) {
        close(in);
        if (fsync(out) != 0 && errno != EINVAL) {
            close(out);
            return -1;
        }
        close(out);
        return 0;
    }
    close(out);
    close(in);
    unlink(dst); /* the O_EXCL placeholder created for the ioctl attempt */
    return gf_fs_copy_file(src, dst, mode);
}

int gf_fs_list_dir(const char *path, char ***out, size_t *count)
{
    *count = 0;
    *out = NULL;
    DIR *d = opendir(path);
    if (!d) {
        /* ENOENT/ENOTDIR is an expected "absent/empty" signal for callers
         * (e.g. first-time payload checks); keep it out of the error log. */
        gf_log_errno(errno == ENOENT || errno == ENOTDIR ? GF_LOG_DEBUG
                                                         : GF_LOG_ERROR,
                     path);
        return -1;
    }
    size_t cap = 8;
    char **list = gf_malloc(cap * sizeof(char *));
    struct dirent *de;
    int rc = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (*count == cap) {
            cap *= 2;
            list = gf_realloc(list, cap * sizeof(char *));
        }
        list[(*count)++] = gf_path_join(path, de->d_name);
    }
    closedir(d);
    *out = list;
    return rc;
}

int gf_fs_hardlink(const char *src, const char *dst)
{
    if (link(src, dst) != 0) {
        gf_log_errno(GF_LOG_DEBUG, "link");
        return -1;
    }
    return 0;
}

int gf_fs_symlink(const char *target, const char *dst)
{
    if (symlink(target, dst) != 0) {
        gf_log_errno(GF_LOG_DEBUG, "symlink");
        return -1;
    }
    return 0;
}

static mode_t sanitize_mode(mode_t m, bool is_dir)
{
    /* strip setuid/setgid and world-write for anything we lay down */
    m &= ~(mode_t)(S_ISUID | S_ISGID | S_ISVTX | S_IWOTH);
    if (is_dir)
        return (mode_t)((m & 0777) | 0755);
    return m & 0777;
}

/* Recursive tree walk driving a per-entry callback with lstat semantics. */
static int tree_walk(const char *src, const char *dst,
                     int (*apply)(const char *s, const char *d,
                                  const struct stat *st, void *ud),
                     void *ud)
{
    struct stat st;
    if (lstat(src, &st) != 0) {
        gf_log_errno(GF_LOG_ERROR, src);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, sanitize_mode(st.st_mode, true)) != 0 && errno != EEXIST) {
            gf_log_errno(GF_LOG_ERROR, dst);
            return -1;
        }
        size_t n = 0;
        char **entries = NULL;
        if (gf_fs_list_dir(src, &entries, &n) != 0)
            return -1;
        /* sort for deterministic application order */
        for (size_t i = 0; i + 1 < n; i++) {
            for (size_t j = i + 1; j < n; j++) {
                if (strcmp(entries[i], entries[j]) > 0) {
                    char *t = entries[i];
                    entries[i] = entries[j];
                    entries[j] = t;
                }
            }
        }
        int rc = 0;
        for (size_t i = 0; i < n && rc == 0; i++) {
            char *s2 = gf_strdup(entries[i]);
            char *b = gf_path_basename(entries[i]);
            char *d2 = gf_path_join(dst, b);
            rc = tree_walk(s2, d2, apply, ud);
            free(s2);
            free(b);
            free(d2);
            if (rc != 0)
                break;
        }
        gf_strv_free(entries, n);
        return rc;
    }
    return apply(src, dst, &st, ud);
}

static int link_one(const char *s, const char *d, const struct stat *st, void *ud)
{
    (void)ud;
    if (S_ISREG(st->st_mode)) {
        if (gf_fs_hardlink(s, d) != 0)
            return -1;
    } else if (S_ISLNK(st->st_mode)) {
        char *tgt = gf_fs_readlink(s);
        if (!tgt)
            return -1;
        int rc = gf_fs_symlink(tgt, d);
        free(tgt);
        if (rc != 0)
            return -1;
    } else {
        gf_log(GF_LOG_WARN, "skipping non-regular file during hardlink: %s", s);
    }
    return 0;
}

static int copy_one(const char *s, const char *d, const struct stat *st, void *ud)
{
    (void)ud;
    if (S_ISREG(st->st_mode)) {
        if (gf_fs_copy_file(s, d, (int)sanitize_mode(st->st_mode, false)) != 0)
            return -1;
        /* preserve mtime (make/build systems depend on it) */
        struct timespec times[2];
        times[0].tv_sec = st->st_atime;
        times[0].tv_nsec = 0;
        times[1].tv_sec = st->st_mtime;
        times[1].tv_nsec = 0;
        utimensat(AT_FDCWD, d, times, AT_SYMLINK_NOFOLLOW);
    } else if (S_ISLNK(st->st_mode)) {
        char *tgt = gf_fs_readlink(s);
        if (!tgt)
            return -1;
        int rc = gf_fs_symlink(tgt, d);
        free(tgt);
        if (rc != 0)
            return -1;
    } else {
        gf_log(GF_LOG_WARN, "skipping non-regular file during copy: %s", s);
    }
    return 0;
}

int gf_fs_hardlink_tree(const char *src, const char *dst)
{
    if (!gf_fs_is_dir(src)) {
        gf_log(GF_LOG_ERROR, "not a directory: %s", src);
        return -1;
    }
    return tree_walk(src, dst, link_one, NULL);
}

int gf_fs_copy_tree(const char *src, const char *dst)
{
    if (!gf_fs_is_dir(src)) {
        gf_log(GF_LOG_ERROR, "not a directory: %s", src);
        return -1;
    }
    return tree_walk(src, dst, copy_one, NULL);
}

/* like copy_tree but skips a top-level ".git" directory */
int gf_fs_copy_tree_skip_git(const char *src, const char *dst)
{
    if (!gf_fs_is_dir(src)) {
        gf_log(GF_LOG_ERROR, "not a directory: %s", src);
        return -1;
    }
    if (gf_fs_mkdir_p(dst) != 0)
        return -1;
    size_t n = 0;
    char **entries = NULL;
    if (gf_fs_list_dir(src, &entries, &n) != 0)
        return -1;
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        char *base = gf_path_basename(entries[i]);
        if (strcmp(base, ".git") == 0) {
            free(base);
            free(entries[i]);
            continue;
        }
        char *d2 = gf_path_join(dst, base);
        struct stat st;
        if (lstat(entries[i], &st) != 0) {
            rc = -1;
        } else if (S_ISDIR(st.st_mode)) {
            rc = tree_walk(entries[i], d2, copy_one, NULL);
        } else {
            rc = copy_one(entries[i], d2, &st, NULL);
        }
        free(d2);
        free(base);
        free(entries[i]);
    }
    free(entries);
    return rc;
}

char *gf_fs_realpath(const char *path)
{
    char buf[4096];
    if (!realpath(path, buf))
        return NULL;
    return gf_strdup(buf);
}

char *gf_fs_readlink(const char *path)
{
    size_t cap = 256;
    for (;;) {
        char *buf = gf_malloc(cap);
        ssize_t n = readlink(path, buf, cap);
        if (n < 0) {
            free(buf);
            gf_log_errno(GF_LOG_DEBUG, "readlink");
            return NULL;
        }
        if ((size_t)n < cap) {
            buf[n] = '\0';
            return buf;
        }
        free(buf);
        cap *= 2;
        if (cap > 1 << 20)
            return NULL;
    }
}

int gf_fs_du(const char *path, uint64_t *out_bytes)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    *out_bytes = (uint64_t)st.st_blocks * 512;
    if (S_ISDIR(st.st_mode)) {
        size_t n = 0;
        char **entries = NULL;
        if (gf_fs_list_dir(path, &entries, &n) != 0)
            return -1;
        for (size_t i = 0; i < n; i++) {
            uint64_t sub = 0;
            if (gf_fs_du(entries[i], &sub) == 0)
                *out_bytes += sub;
            free(entries[i]);
        }
        free(entries);
    }
    return 0;
}

size_t gf_hex_encode(char *dst, size_t dstlen, const uint8_t *src, size_t n)
{
    static const char hexdig[] = "0123456789abcdef";
    if (dstlen < 2 * n + 1)
        return 0;
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = hexdig[src[i] >> 4];
        dst[2 * i + 1] = hexdig[src[i] & 0x0F];
    }
    dst[2 * n] = '\0';
    return 2 * n;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int gf_hex_decode(uint8_t *dst, size_t dstlen, const char *hex, size_t n)
{
    if (n % 2 != 0 || dstlen < n / 2)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int gf_ct_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(pa[i] ^ pb[i]);
    return diff != 0;
}

int gf_mkstemp_in(const char *dir, char **path)
{
    char *tmpl = gf_path_join(dir, ".tmp.XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        gf_log_errno(GF_LOG_ERROR, "mkstemp");
        free(tmpl);
        return -1;
    }
    *path = tmpl;
    return fd;
}

void gf_time_iso8601_now(char *buf, size_t buflen)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || buflen < 21) {
        snprintf(buf, buflen, "1970-01-01T00:00:00Z");
        return;
    }
    struct tm tm;
    time_t sec = ts.tv_sec;
    if (!gmtime_r(&sec, &tm)) {
        snprintf(buf, buflen, "1970-01-01T00:00:00Z");
        return;
    }
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int64_t gf_time_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

char **gf_argv_dup(const char *const *argv)
{
    size_t n = gf_argv_len(argv);
    char **out = gf_malloc((n + 1) * sizeof(char *));
    for (size_t i = 0; i < n; i++)
        out[i] = gf_strdup(argv[i]);
    out[n] = NULL;
    return out;
}

size_t gf_argv_len(const char *const *argv)
{
    size_t n = 0;
    while (argv && argv[n])
        n++;
    return n;
}
