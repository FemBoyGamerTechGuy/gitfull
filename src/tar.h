/* tar.h — hardened tar (ustar/pax/GNU) reader + deterministic writer.
 *
 * The reader is the main attack surface for untrusted forge archives. It:
 *  - rejects absolute paths, ".." components, and NUL tricks
 *  - rejects device nodes, fifos, sockets, and sparse files
 *  - rejects symlink/hardlink targets that escape the extraction root
 *  - rejects duplicate paths (TOCTOU / overwrite resistance)
 *  - strips setuid/setgid and world-writable bits
 *  - enforces entry/total size and entry count limits
 *  - never follows symlinks while extracting (O_NOFOLLOW|O_EXCL)
 * gzip (and plain) input is handled via zlib streaming.
 */
#ifndef GF_TAR_H
#define GF_TAR_H

#include <stdint.h>
#include <stddef.h>

typedef struct gf_tar_limits {
    uint64_t max_total_bytes;  /* 0 = default 1 GiB */
    uint64_t max_entry_bytes;  /* 0 = default 1 GiB */
    uint64_t max_entries;      /* 0 = default 200000 */
} gf_tar_limits;

gf_tar_limits gf_tar_limits_default(void);

/* Extract archive into dest_dir (must exist, should be empty). If strip >= 0,
 * that many leading path components are removed from every entry (forge
 * tarballs embed a top-level "<repo>-<tag>/" directory). strip=-1 = auto
 * (strip single common top-level dir when every entry shares one).
 * Returns 0 on success; on policy violation returns -1 after logging.
 * *out_nfiles receives number of extracted files (may be NULL). */
int gf_tar_extract(const char *archive_path, const char *dest_dir, int strip,
                   const gf_tar_limits *limits, size_t *out_nfiles);

typedef struct gf_tar_entry {
    char *path;
    char type;        /* 'f' file, 'd' dir, 'l' symlink, 'h' hardlink */
    char *target;     /* symlink/hardlink target or NULL */
    uint64_t size;
    uint32_t mode;    /* sanitized */
} gf_tar_entry;

/* List entries WITHOUT extracting (same validation as extraction). */
int gf_tar_list(const char *archive_path, gf_tar_entry **entries, size_t *n,
                const gf_tar_limits *limits);
void gf_tar_entries_free(gf_tar_entry *entries, size_t n);

/* Write a deterministic ustar/pax tar of src_dir contents into out_path.
 * Every entry is prefixed with 'prefix' (e.g. "payload") when non-NULL.
 * Determinism: entries sorted lexicographically, uid/gid = 0, uname/gname
 * empty, mtime = epoch (SOURCE_DATE_EPOCH semantics; caller passes it),
 * modes sanitized, no atime/ctime. */
int gf_tar_write_dir(const char *src_dir, const char *out_path,
                     const char *prefix, uint64_t epoch);

#endif /* GF_TAR_H */
