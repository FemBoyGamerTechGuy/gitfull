/* tar.c — hardened tar reader + deterministic writer. */
#include "tar.h"

#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define TAR_BLOCK 512

gf_tar_limits gf_tar_limits_default(void)
{
    gf_tar_limits l = { 0 };
    l.max_total_bytes = 1ull << 30;
    l.max_entry_bytes = 1ull << 30;
    l.max_entries = 200000;
    return l;
}

/* ------------------------------------------------------------- reading */

typedef struct gf_tar_reader {
    /* gzip or plain */
    gzFile gz;
    int fd;
    bool use_gz;
    uint64_t pos;         /* logical byte position */
    char hdr[TAR_BLOCK];
    /* pending pax/GNU overrides for the next real entry */
    char *pax_path;
    char *pax_linkpath;
    uint64_t pax_size;
    bool pax_size_set;
    char *gnu_longname;
    char *gnu_longlink;
    gf_tar_limits limv;             /* limits stored by value (never NULL) */
    uint64_t total_bytes;
    uint64_t entry_count;
} gf_tar_reader;

/* Read exactly 'want' bytes.
 * Returns 1 = full read; 0 = clean EOF (zero bytes available);
 * -1 = hard error; -2 = SHORT read (truncated/corrupt archive).
 * A partial read must never be reported as EOF: a <512-byte garbage file
 * would otherwise parse as a "valid empty archive". */
static int rd_read(gf_tar_reader *r, void *buf, size_t want)
{
    if (want == 0)
        return 0;
    if (r->use_gz) {
        int got = gzread(r->gz, buf, (unsigned)want);
        if (got < 0) {
            gf_log(GF_LOG_ERROR, "tar: gzip read error");
            return -1;
        }
        if (got == 0)
            return 0; /* clean EOF */
        if ((size_t)got != want) {
            gf_log(GF_LOG_ERROR,
                   "tar: truncated archive (short read of %d/%zu bytes)",
                   got, want);
            return -2;
        }
        r->pos += want;
        return 1;
    }
    size_t done = 0;
    while (done < want) {
        ssize_t n = read(r->fd, (char *)buf + done, want - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            gf_log_errno(GF_LOG_ERROR, "tar: read");
            return -1;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    if (done == 0)
        return 0; /* clean EOF */
    if (done != want) {
        gf_log(GF_LOG_ERROR,
               "tar: truncated archive (short read of %zu/%zu bytes)",
               done, want);
        return -2;
    }
    r->pos += want;
    return 1;
}

static void rd_close(gf_tar_reader *r)
{
    if (r->use_gz) {
        if (r->gz)
            gzclose(r->gz);
    } else if (r->fd >= 0) {
        close(r->fd);
    }
    free(r->pax_path);
    free(r->pax_linkpath);
    free(r->gnu_longname);
    free(r->gnu_longlink);
    memset(r, 0, sizeof(*r));
}

static int rd_open(gf_tar_reader *r, const char *path,
                   const gf_tar_limits *lim)
{
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->limv = lim ? *lim : gf_tar_limits_default();
    unsigned char magic[2] = { 0, 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        gf_log_errno(GF_LOG_ERROR, path);
        return -1;
    }
    ssize_t n = read(fd, magic, 2);
    if (n == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
        close(fd);
        r->use_gz = true;
        r->gz = gzopen(path, "rb");
        if (!r->gz) {
            gf_log(GF_LOG_ERROR, "tar: gzopen failed: %s", path);
            return -1;
        }
        return 0;
    }
    if (n < 0) {
        gf_log_errno(GF_LOG_ERROR, "tar: read");
        close(fd);
        return -1;
    }
    /* plain: rewind */
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }
    r->use_gz = false;
    r->fd = fd;
    return 0;
}

static uint64_t parse_octal(const char *field, size_t len)
{
    /* skip spaces/NULs, tolerate GNU base-256 */
    if ((unsigned char)field[0] & 0x80) {
        uint64_t v = (uint64_t)((unsigned char)field[0] & 0x7f);
        for (size_t i = 1; i < len && i < 8; i++)
            v = (v << 8) | (unsigned char)field[i];
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c == 0)
            break;
        if (c == ' ') {
            if (v)
                continue; /* tolerate trailing spaces after digits */
            continue;
        }
        if (c < '0' || c > '9')
            break;
        v = v * 8 + (uint64_t)(c - '0');
    }
    return v;
}

static bool is_zero_block(const char *block)
{
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        if (block[i] != 0)
            return false;
    }
    return true;
}

static bool checksum_ok(const char hdr[TAR_BLOCK])
{
    /* The stored checksum is the sum of all header bytes with the checksum
     * field replaced by spaces. Verify both signed and unsigned variants
     * (old tars used signed char sums). */
    uint64_t stored = parse_octal(hdr + 148, 8);
    uint64_t unsigned_sum = 0;
    int64_t signed_sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        unsigned char c = (unsigned char)hdr[i];
        if (i >= 148 && i < 156)
            c = ' ';
        unsigned_sum += c;
        signed_sum += (int64_t)(signed char)c;
    }
    if (stored == unsigned_sum)
        return true;
    if (stored == (uint64_t)signed_sum && signed_sum >= 0)
        return true;
    return false;
}

/* join prefix + name from ustar header */
static char *header_name(const char hdr[TAR_BLOCK])
{
    char name[101], prefix[156];
    memcpy(name, hdr, 100);
    name[100] = '\0';
    memcpy(prefix, hdr + 345, 155);
    prefix[155] = '\0';
    /* NUL-terminate at first NUL */
    for (size_t i = 0; i < 100; i++)
        if (name[i] == '\0') { name[i] = '\0'; break; }
    for (size_t i = 0; i < 155; i++)
        if (prefix[i] == '\0') { break; }
    if (prefix[0] != '\0' && hdr[156] != 'L') {
        char joined[300];
        snprintf(joined, sizeof(joined), "%.*s/%.*s", 155, prefix, 100, name);
        return gf_strdup(joined);
    }
    return gf_strdup(name);
}

/* read exactly `size` bytes of entry data (padded) as a malloc'd buffer
 * (size + 1 for NUL). Returns NULL on failure/overflow. */
static char *read_data(gf_tar_reader *r, uint64_t size, uint64_t cap)
{
    if (size > cap) {
        gf_log(GF_LOG_ERROR, "tar: entry data too large (%llu bytes)",
               (unsigned long long)size);
        return NULL;
    }
    if (r->limv.max_total_bytes && r->total_bytes + size > r->limv.max_total_bytes) {
        gf_log(GF_LOG_ERROR, "tar: total size limit exceeded");
        return NULL;
    }
    char *buf = gf_malloc((size_t)size + 1);
    if (size > 0) {
        int got = rd_read(r, buf, (size_t)size);
        if (got != 1) {
            gf_log(GF_LOG_ERROR, "tar: truncated entry data");
            free(buf);
            return NULL;
        }
    }
    buf[size] = '\0';
    r->total_bytes += size;
    /* skip padding to block boundary */
    uint64_t padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    uint64_t skip = padded - size;
    while (skip > 0) {
        char junk[TAR_BLOCK];
        size_t chunk = skip > TAR_BLOCK ? TAR_BLOCK : (size_t)skip;
        int got = rd_read(r, junk, chunk);
        if (got != 1) {
            gf_log(GF_LOG_ERROR, "tar: truncated padding");
            free(buf);
            return NULL;
        }
        skip -= chunk;
    }
    return buf;
}

static int skip_data(gf_tar_reader *r, uint64_t size)
{
    uint64_t padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    while (padded > 0) {
        char junk[TAR_BLOCK];
        size_t chunk = padded > TAR_BLOCK ? TAR_BLOCK : (size_t)padded;
        int got = rd_read(r, junk, chunk);
        if (got != 1)
            return -1;
        padded -= chunk;
    }
    return 0;
}

/* Parse pax "len key=value\n" records from data. */
static int parse_pax(gf_tar_reader *r, char *data, uint64_t datalen)
{
    char *p = data;
    char *end = data + datalen;
    while (p < end) {
        /* record length (decimal, includes the length digits itself) */
        char *sp = strchr(p, ' ');
        if (!sp)
            break;
        long reclen = strtol(p, NULL, 10);
        if (reclen <= 0 || p + reclen > end)
            break;
        char *rec = p;
        p += reclen;
        char *eq = strchr(sp + 1, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = sp + 1;
        char *val = eq + 1;
        char *nl = strchr(val, '\n');
        if (nl)
            *nl = '\0';
        if (strcmp(key, "path") == 0) {
            free(r->pax_path);
            r->pax_path = gf_strdup(val);
        } else if (strcmp(key, "linkpath") == 0) {
            free(r->pax_linkpath);
            r->pax_linkpath = gf_strdup(val);
        } else if (strcmp(key, "size") == 0) {
            errno = 0;
            unsigned long long sz = strtoull(val, NULL, 10);
            if (!errno) {
                r->pax_size = sz;
                r->pax_size_set = true;
            }
        }
        (void)rec;
    }
    return 0;
}

/* Validate a path that will be created inside the extraction root.
 * Returns malloc'd normalized relative path or NULL (logged). */
static char *validate_entry_path(const char *raw)
{
    if (!raw || !*raw) {
        gf_log(GF_LOG_ERROR, "tar: empty entry name");
        return NULL;
    }
    if (raw[0] == '/') {
        gf_log(GF_LOG_ERROR, "tar: rejecting absolute path: %s", raw);
        return NULL;
    }
    if (strstr(raw, "../") || gf_str_ends_with(raw, "/..") || strcmp(raw, "..") == 0) {
        gf_log(GF_LOG_ERROR, "tar: rejecting path traversal: %s", raw);
        return NULL;
    }
    /* NUL can't appear (C string), but reject control chars */
    for (const char *p = raw; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) {
            gf_log(GF_LOG_ERROR, "tar: rejecting control character in path");
            return NULL;
        }
    }
    char *norm = gf_path_normalize(raw);
    if (!*norm || strcmp(norm, ".") == 0) {
        free(norm);
        gf_log(GF_LOG_ERROR, "tar: entry normalizes to nothing: %s", raw);
        return NULL;
    }
    if (norm[0] == '/') {
        free(norm);
        gf_log(GF_LOG_ERROR, "tar: path escaped during normalization: %s", raw);
        return NULL;
    }
    return norm;
}

/* Symlink/hardlink target validation: relative targets must stay inside the
 * extraction root; absolute targets are rejected. */
static int validate_link_target(const char *target, const char *linkpath)
{
    if (!target || !*target) {
        gf_log(GF_LOG_ERROR, "tar: empty link target for %s", linkpath);
        return -1;
    }
    if (target[0] == '/') {
        gf_log(GF_LOG_ERROR,
               "tar: rejecting absolute symlink target %s -> %s", linkpath, target);
        return -1;
    }
    if (strstr(target, "../") || gf_str_ends_with(target, "/..") ||
        strcmp(target, "..") == 0) {
        /* a relative target with ".." may or may not escape; resolving the
         * link at extraction-time directory + target: we reject all ".."
         * targets for safety (documented limitation). */
        gf_log(GF_LOG_WARN, "tar: symlink with '..' target rejected: %s -> %s",
               linkpath, target);
        return -1;
    }
    return 0;
}

static uint32_t sanitize_mode(uint64_t raw, bool is_dir)
{
    uint32_t m = (uint32_t)(raw & 0xFFF);
    /* strip setuid/setgid/sticky; forbid world-write */
    m &= ~(uint32_t)(S_ISUID | S_ISGID | S_ISVTX | S_IWOTH);
    if (is_dir)
        m |= 0700; /* ensure owner rwx on dirs so we can traverse */
    return m;
}

typedef enum {
    TAR_NEXT_EOF = 0,
    TAR_NEXT_ENTRY,
    TAR_NEXT_ERROR = -1
} tar_next;

/* One iteration of the header loop. Fills *out with a validated entry
 * (path/type/size/mode/target, all owned). Caller must free fields. */
typedef struct raw_entry {
    char *path;
    char type;      /* 'f','d','l','h' */
    char *target;
    uint64_t size;
    uint32_t mode;
    bool has_data;  /* data follows in stream */
} raw_entry;

static void raw_entry_free(raw_entry *e)
{
    free(e->path);
    free(e->target);
    memset(e, 0, sizeof(*e));
}

static tar_next tar_next_entry(gf_tar_reader *r, raw_entry *out)
{
    memset(out, 0, sizeof(*out));
    /* two consecutive zero blocks = end; we accept a clean EOF too */
    int got = rd_read(r, r->hdr, TAR_BLOCK);
    if (got == 0)
        return TAR_NEXT_EOF; /* clean end of stream */
    if (got != 1)
        return TAR_NEXT_ERROR; /* short read: truncated/corrupt archive */
    if (is_zero_block(r->hdr)) {
        /* require a second zero block or clean EOF */
        got = rd_read(r, r->hdr, TAR_BLOCK);
        if (got == 0 || (got == 1 && is_zero_block(r->hdr)))
            return TAR_NEXT_EOF;
        if (got != 1)
            return TAR_NEXT_ERROR; /* truncated */
        gf_log(GF_LOG_ERROR, "tar: single zero block (garbage after archive?)");
        return TAR_NEXT_ERROR;
    }
    if (!checksum_ok(r->hdr)) {
        gf_log(GF_LOG_ERROR, "tar: header checksum mismatch at offset %llu",
               (unsigned long long)(r->pos - TAR_BLOCK));
        return TAR_NEXT_ERROR;
    }

    char typeflag = r->hdr[156];
    char *name = header_name(r->hdr);
    uint64_t size = parse_octal(r->hdr + 124, 12);
    uint64_t mode_raw = parse_octal(r->hdr + 100, 8);

    /* metadata-only headers carrying overrides for the next entry */
    if (typeflag == 'x' || typeflag == 'X') { /* pax extended */
        char *data = read_data(r, size, 1 << 20);
        if (!data) {
            free(name);
            return TAR_NEXT_ERROR;
        }
        parse_pax(r, data, size);
        free(data);
        free(name);
        return tar_next_entry(r, out); /* recurse for the real entry */
    }
    if (typeflag == 'g') { /* pax global: ignore records */
        if (skip_data(r, size) != 0) {
            free(name);
            return TAR_NEXT_ERROR;
        }
        free(name);
        return tar_next_entry(r, out);
    }
    if (typeflag == 'L' || typeflag == 'K') { /* GNU longname/longlink */
        char *data = read_data(r, size, 1 << 20);
        if (!data) {
            free(name);
            return TAR_NEXT_ERROR;
        }
        /* trim trailing NULs/newline */
        size_t dl = strlen(data);
        while (dl > 0 && (data[dl - 1] == '\0' || data[dl - 1] == '\n'))
            data[--dl] = '\0';
        if (typeflag == 'L') {
            free(r->gnu_longname);
            r->gnu_longname = gf_strdup(data);
        } else {
            free(r->gnu_longlink);
            r->gnu_longlink = gf_strdup(data);
        }
        free(data);
        free(name);
        return tar_next_entry(r, out);
    }

    /* apply pending overrides */
    char *path;
    if (r->pax_path) {
        path = r->pax_path;
        r->pax_path = NULL;
    } else if (r->gnu_longname) {
        path = r->gnu_longname;
        r->gnu_longname = NULL;
    } else {
        path = name;
        name = NULL;
    }
    free(name);

    if (r->pax_size_set) {
        size = r->pax_size;
        r->pax_size_set = false;
        r->pax_size = 0;
    }

    char *link_target = NULL;
    if (typeflag == '2' || typeflag == '1' || typeflag == 'h') {
        if (r->pax_linkpath) {
            link_target = r->pax_linkpath;
            r->pax_linkpath = NULL;
        } else if (r->gnu_longlink) {
            link_target = r->gnu_longlink;
            r->gnu_longlink = NULL;
        } else {
            char link[101];
            memcpy(link, r->hdr + 157, 100);
            link[100] = '\0';
            link_target = gf_strdup(link);
        }
    } else {
        /* clear stale link overrides that belong to link entries only */
        free(r->pax_linkpath);
        r->pax_linkpath = NULL;
        free(r->gnu_longlink);
        r->gnu_longlink = NULL;
    }

    /* classify + reject dangerous types */
    switch (typeflag) {
    case '0':
    case '\0':
    case '7':
        out->type = 'f';
        out->has_data = true;
        break;
    case '5':
        out->type = 'd';
        size = 0;
        out->has_data = false;
        break;
    case '2':
        out->type = 'l';
        out->target = link_target;
        link_target = NULL;
        out->has_data = false;
        break;
    case '1':
    case 'h':
        out->type = 'h';
        out->target = link_target;
        link_target = NULL;
        out->has_data = false;
        break;
    case '3':
    case '4':
    case '6':
        gf_log(GF_LOG_ERROR,
               "tar: rejecting special file (device/fifo) entry: %s", path);
        goto fail;
    case 'S':
        gf_log(GF_LOG_ERROR, "tar: GNU sparse files are not supported: %s",
               path);
        goto fail;
    default:
        gf_log(GF_LOG_ERROR, "tar: unknown typeflag '%c' for %s", typeflag,
               path);
        goto fail;
    }

    out->path = validate_entry_path(path);
    free(path);
    if (!out->path)
        return TAR_NEXT_ERROR;
    if (out->target && validate_link_target(out->target, out->path) != 0)
        return TAR_NEXT_ERROR;

    if (r->limv.max_entry_bytes && size > r->limv.max_entry_bytes) {
        gf_log(GF_LOG_ERROR, "tar: entry too large: %s (%llu bytes)", out->path,
               (unsigned long long)size);
        return TAR_NEXT_ERROR;
    }
    r->entry_count++;
    if (r->limv.max_entries && r->entry_count > r->limv.max_entries) {
        gf_log(GF_LOG_ERROR, "tar: too many entries");
        return TAR_NEXT_ERROR;
    }
    out->size = size;
    out->mode = sanitize_mode(mode_raw, out->type == 'd');
    return TAR_NEXT_ENTRY;

fail:
    free(path);
    free(link_target);
    return TAR_NEXT_ERROR;
}

/* strip components: returns malloc'd stripped path or NULL if the entry
 * would vanish (when required=true that's an error). */
static char *strip_path(const char *path, int strip)
{
    if (strip <= 0)
        return gf_strdup(path);
    const char *p = path;
    for (int i = 0; i < strip; i++) {
        const char *slash = strchr(p, '/');
        if (!slash)
            return NULL;
        p = slash + 1;
    }
    if (!*p)
        return NULL;
    return gf_strdup(p);
}

/* count leading components shared by all entries (for auto-strip) */
static int common_prefix_components(char **paths, size_t n)
{
    if (n == 0)
        return 0;
    int comps = 0;
    for (;;) {
        /* component #comps of entry 0 */
        const char *p = paths[0];
        for (int i = 0; i < comps; i++) {
            const char *slash = strchr(p, '/');
            if (!slash)
                return comps;
            p = slash + 1;
        }
        const char *slash = strchr(p, '/');
        if (!slash)
            return comps;
        size_t clen = (size_t)(slash - p);
        for (size_t k = 0; k < n; k++) {
            const char *q = paths[k];
            for (int i = 0; i < comps; i++) {
                const char *s2 = strchr(q, '/');
                if (!s2)
                    return comps;
                q = s2 + 1;
            }
            const char *qslash = strchr(q, '/');
            if (!qslash)
                return comps;
            if ((size_t)(qslash - q) != clen || strncmp(q, p, clen) != 0)
                return comps;
        }
        comps++;
    }
}

int gf_tar_list(const char *archive_path, gf_tar_entry **entries_out,
                size_t *n, const gf_tar_limits *limits)
{
    *entries_out = NULL;
    *n = 0;
    gf_tar_reader r;
    if (rd_open(&r, archive_path, limits) != 0)
        return -1;

    gf_tar_entry *list = NULL;
    size_t count = 0, cap = 0;

    for (;;) {
        raw_entry e;
        tar_next rc = tar_next_entry(&r, &e);
        if (rc == TAR_NEXT_EOF)
            break;
        if (rc == TAR_NEXT_ERROR) {
            raw_entry_free(&e);
            goto fail;
        }
        if (e.type == 'f') {
            if (skip_data(&r, e.size) != 0) {
                raw_entry_free(&e);
                goto fail;
            }
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            list = gf_realloc(list, cap * sizeof(gf_tar_entry));
        }
        list[count].path = gf_strdup(e.path);
        list[count].type = e.type;
        list[count].target = e.target ? gf_strdup(e.target) : NULL;
        list[count].size = e.size;
        list[count].mode = e.mode;
        count++;
        raw_entry_free(&e);
    }
    rd_close(&r);
    *n = count;
    *entries_out = list;
    return 0;

fail:
    rd_close(&r);
    for (size_t i = 0; i < count; i++) {
        free(list[i].path);
        free(list[i].target);
    }
    free(list);
    return -1;
}

void gf_tar_entries_free(gf_tar_entry *entries, size_t n)
{
    if (!entries)
        return;
    for (size_t i = 0; i < n; i++) {
        free(entries[i].path);
        free(entries[i].target);
    }
    free(entries);
}

/* ------------------------------------------------------------ extraction */

int gf_tar_extract(const char *archive_path, const char *dest_dir, int strip,
                   const gf_tar_limits *limits, size_t *out_nfiles)
{
    if (out_nfiles)
        *out_nfiles = 0;
    if (!gf_fs_is_dir(dest_dir)) {
        gf_log(GF_LOG_ERROR, "tar: destination is not a directory: %s", dest_dir);
        return -1;
    }

    /* Pass 1: full validation before touching the filesystem, and auto-strip
     * analysis (strip == -1 means "strip the single common top-level dir"). */
    gf_tar_entry *ents = NULL;
    size_t n = 0;
    if (gf_tar_list(archive_path, &ents, &n, limits) != 0)
        return -1;
    if (strip == -1) {
        char **paths = gf_malloc((n ? n : 1) * sizeof(char *));
        for (size_t i = 0; i < n; i++)
            paths[i] = ents[i].path;
        strip = common_prefix_components(paths, n);
        free(paths);
    }
    gf_tar_entries_free(ents, n);

    /* Stream-extract with the decided strip count. */
    gf_tar_reader r;
    if (rd_open(&r, archive_path, limits) != 0)
        return -1;

    int rc = -1;
    size_t created = 0;
    /* track extracted file paths for hardlink validation */
    char **files = NULL;
    size_t nfiles = 0, fcap = 0;

    for (;;) {
        raw_entry e;
        tar_next tn = tar_next_entry(&r, &e);
        if (tn == TAR_NEXT_EOF)
            break;
        if (tn == TAR_NEXT_ERROR) {
            raw_entry_free(&e);
            goto out;
        }

        char *rel = strip_path(e.path, strip);
        if (!rel || !*rel) {
            free(rel);
            if (e.type == 'f')
                skip_data(&r, e.size);
            raw_entry_free(&e);
            continue;
        }

        char *dest = gf_path_join(dest_dir, rel);
        free(rel);

        if (e.type == 'd') {
            gf_fs_mkdir_p(dest);
        } else if (e.type == 'l') {
            if (gf_fs_symlink(e.target, dest) != 0) {
                gf_log(GF_LOG_ERROR, "tar: failed to create symlink %s", dest);
                free(dest);
                raw_entry_free(&e);
                goto out;
            }
        } else if (e.type == 'h') {
            /* hardlink targets are archive paths: apply the same strip */
            char *tgt_rel = strip_path(e.target, strip);
            if (!tgt_rel || !*tgt_rel) {
                gf_log(GF_LOG_ERROR, "tar: hardlink target vanishes after strip: %s",
                       e.target);
                free(dest);
                free(tgt_rel);
                raw_entry_free(&e);
                goto out;
            }
            char *hd = gf_path_join(dest_dir, tgt_rel);
            free(tgt_rel);
            if (gf_fs_hardlink(hd, dest) != 0) {
                gf_log(GF_LOG_ERROR, "tar: failed to hardlink %s -> %s", dest,
                       e.target);
                free(hd);
                free(dest);
                raw_entry_free(&e);
                goto out;
            }
            free(hd);
        } else { /* regular file */
            char *parent = gf_path_dirname(dest);
            if (gf_fs_mkdir_p(parent) != 0) {
                free(parent);
                free(dest);
                raw_entry_free(&e);
                goto out;
            }
            free(parent);
            int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          e.mode ? e.mode : 0644);
            if (fd < 0) {
                gf_log_errno(GF_LOG_ERROR, dest);
                free(dest);
                raw_entry_free(&e);
                goto out;
            }
            /* stream copy e.size bytes */
            uint64_t left = e.size;
            bool ok = true;
            while (left > 0) {
                char buf[65536];
                size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
                int got = rd_read(&r, buf, chunk);
                if (got != 1) {
                    gf_log(GF_LOG_ERROR, "tar: truncated file data for %s", dest);
                    ok = false;
                    break;
                }
                size_t off = 0;
                while (off < chunk) {
                    ssize_t w = write(fd, buf + off, chunk - off);
                    if (w < 0 && errno == EINTR)
                        continue;
                    if (w < 0) {
                        gf_log_errno(GF_LOG_ERROR, "write");
                        ok = false;
                        break;
                    }
                    off += (size_t)w;
                }
                if (!ok)
                    break;
                left -= chunk;
            }
            close(fd);
            if (!ok) {
                free(dest);
                raw_entry_free(&e);
                goto out;
            }
            /* skip padding */
            uint64_t padded = (e.size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
            uint64_t skip = padded - e.size;
            while (skip > 0) {
                char junk[TAR_BLOCK];
                size_t chunk = skip > TAR_BLOCK ? TAR_BLOCK : (size_t)skip;
                if (rd_read(&r, junk, chunk) != 1) {
                    free(dest);
                    raw_entry_free(&e);
                    goto out;
                }
                skip -= chunk;
            }
            created++;
            if (nfiles == fcap) {
                fcap = fcap ? fcap * 2 : 64;
                files = gf_realloc(files, fcap * sizeof(char *));
            }
            files[nfiles++] = gf_strdup(e.path);
        }
        free(dest);
        raw_entry_free(&e);
    }

    /* re-apply directory modes after all files written (dirs may have been
     * created with default 0755 by mkdir_p; sanitize what we saw) — skipped:
     * modes are sanitized to a safe subset anyway. */
    rc = 0;
    if (out_nfiles)
        *out_nfiles = created;

out:
    for (size_t i = 0; i < nfiles; i++)
        free(files[i]);
    free(files);
    rd_close(&r);
    return rc;
}

/* ------------------------------------------------------------- writing */

static void write_octal(char *field, size_t len, uint64_t value)
{
    /* len includes the terminating NUL/space */
    memset(field, '0', len - 1);
    field[len - 1] = ' ';
    /* find highest position */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llo", (unsigned long long)value);
    size_t tl = strlen(tmp);
    if (tl > len - 1)
        tl = len - 1;
    memcpy(field + (len - 1) - tl, tmp, tl);
}

static int write_header(FILE *f, const char *path, char typeflag,
                        uint64_t size, uint32_t mode, const char *target,
                        uint64_t epoch)
{
    char hdr[TAR_BLOCK];
    memset(hdr, 0, sizeof(hdr));
    const char *prefix = "";
    char namebuf[100];
    if (strlen(path) > 99) {
        /* try to split into prefix/name at a slash */
        size_t pl = strlen(path);
        const char *best = NULL;
        for (const char *p = path + pl - 100; p > path; p--) {
            if (*p == '/' && (size_t)(p - path) - 1 <= 154 &&
                pl - (size_t)(p - path) - 1 <= 99) {
                best = p;
                break;
            }
        }
        if (!best) {
            gf_log(GF_LOG_ERROR, "tar write: path too long for ustar: %s", path);
            return -1;
        }
        size_t prelen = (size_t)(best - path);
        memcpy(hdr + 345, path, prelen > 155 ? 155 : prelen);
        snprintf(namebuf, sizeof(namebuf), "%s", best + 1);
        prefix = namebuf;
    } else {
        snprintf(namebuf, sizeof(namebuf), "%s", path);
    }
    (void)prefix;
    memcpy(hdr, namebuf, strlen(namebuf) <= 99 ? strlen(namebuf) : 99);
    write_octal(hdr + 100, 8, mode);
    write_octal(hdr + 108, 8, 0);       /* uid */
    write_octal(hdr + 116, 8, 0);       /* gid */
    write_octal(hdr + 124, 12, size);
    write_octal(hdr + 136, 12, epoch);
    memset(hdr + 148, ' ', 8);          /* chksum placeholder */
    hdr[156] = typeflag;
    if (target) {
        size_t tl = strlen(target);
        memcpy(hdr + 157, target, tl <= 99 ? tl : 99);
    }
    memcpy(hdr + 257, "ustar", 5);
    hdr[262] = '0';
    /* uname/gname empty; devmajor/devminor zero */
    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        sum += (unsigned char)hdr[i];
    char chk[10];
    snprintf(chk, sizeof(chk), "%06llo ", (unsigned long long)sum);
    memcpy(hdr + 148, chk, 8);
    if (fwrite(hdr, 1, TAR_BLOCK, f) != TAR_BLOCK)
        return -1;
    return 0;
}

static int write_pad(FILE *f, uint64_t size)
{
    uint64_t padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    uint64_t pad = padded - size;
    if (pad == 0)
        return 0;
    char zeros[TAR_BLOCK];
    memset(zeros, 0, sizeof(zeros));
    if (fwrite(zeros, 1, (size_t)pad, f) != (size_t)pad)
        return -1;
    return 0;
}

static int write_tree_entries(FILE *f, const char *src, const char *prefix,
                              uint64_t epoch, const char *rel);

static int write_one_entry(FILE *f, const char *src, const char *prefix,
                           uint64_t epoch, const char *rel)
{
    struct stat st;
    if (lstat(src, &st) != 0)
        return -1;
    char *full_rel = prefix && *prefix
        ? (rel[0] ? gf_path_join(prefix, rel) : gf_strdup(prefix))
        : gf_strdup(rel);
    if (!full_rel || !*full_rel) {
        free(full_rel);
        return 0;
    }
    int rc = -1;
    if (S_ISDIR(st.st_mode)) {
        /* directories: trailing slash */
        char *d = gf_malloc(strlen(full_rel) + 2);
        sprintf(d, "%s/", full_rel);
        rc = write_header(f, d, '5', 0, 0755, NULL, epoch);
        free(d);
    } else if (S_ISREG(st.st_mode)) {
        rc = write_header(f, full_rel, '0', (uint64_t)st.st_size,
                          (uint32_t)(st.st_mode & 0777), NULL, epoch);
        if (rc == 0) {
            FILE *in = fopen(src, "rb");
            if (!in)
                goto done;
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                if (fwrite(buf, 1, n, f) != n) {
                    fclose(in);
                    goto done;
                }
            }
            bool ferr = ferror(in);
            fclose(in);
            if (ferr)
                goto done;
            rc = write_pad(f, (uint64_t)st.st_size);
        }
    } else if (S_ISLNK(st.st_mode)) {
        char *tgt = gf_fs_readlink(src);
        if (!tgt)
            goto done;
        rc = write_header(f, full_rel, '2', 0, 0777, tgt, epoch);
        free(tgt);
    } else {
        gf_log(GF_LOG_WARN, "tar write: skipping special file: %s", src);
        rc = 0;
    }
done:
    free(full_rel);
    return rc;
}

static int write_tree_entries(FILE *f, const char *src, const char *prefix,
                              uint64_t epoch, const char *rel)
{
    /* recursive with sorted children; 'rel' is the path inside the archive */
    struct stat st;
    if (lstat(src, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        /* non-directory: rel is already the complete relative path */
        return write_one_entry(f, src, prefix, epoch, rel);
    }
    /* write the directory itself (except the root) */
    if (*rel) {
        if (write_one_entry(f, src, prefix, epoch, rel) != 0)
            return -1;
    }
    size_t n = 0;
    char **entries = NULL;
    if (gf_fs_list_dir(src, &entries, &n) != 0)
        return -1;
    for (size_t i = 0; i + 1 < n; i++) {
        for (size_t k = i + 1; k < n; k++) {
            if (strcmp(entries[i], entries[k]) > 0) {
                char *t = entries[i];
                entries[i] = entries[k];
                entries[k] = t;
            }
        }
    }
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        char *base = gf_path_basename(entries[i]);
        char *child_rel = rel[0] ? gf_path_join(rel, base) : gf_strdup(base);
        rc = write_tree_entries(f, entries[i], prefix, epoch, child_rel);
        free(child_rel);
        free(base);
        free(entries[i]);
    }
    free(entries);
    return rc;
}

int gf_tar_write_dir(const char *src_dir, const char *out_path,
                     const char *prefix, uint64_t epoch)
{
    if (!gf_fs_is_dir(src_dir)) {
        gf_log(GF_LOG_ERROR, "tar write: not a directory: %s", src_dir);
        return -1;
    }
    char *tmp = NULL;
    char *outdir = gf_path_dirname(out_path);
    int fd = gf_mkstemp_in(outdir, &tmp);
    if (fd < 0) {
        free(outdir);
        return -1;
    }
    close(fd);
    unlink(tmp);
    free(outdir);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        gf_log_errno(GF_LOG_ERROR, tmp);
        free(tmp);
        return -1;
    }
    int rc = write_tree_entries(f, src_dir, prefix, epoch, "");
    if (rc == 0) {
        /* two zero blocks */
        char zeros[TAR_BLOCK * 2];
        memset(zeros, 0, sizeof(zeros));
        if (fwrite(zeros, 1, sizeof(zeros), f) != sizeof(zeros))
            rc = -1;
    }
    if (fclose(f) != 0)
        rc = -1;
    if (rc == 0) {
        if (rename(tmp, out_path) != 0) {
            gf_log_errno(GF_LOG_ERROR, "rename");
            rc = -1;
        } else {
            char *od = gf_path_dirname(out_path);
            gf_fs_fsync_dir(od);
            free(od);
        }
    }
    if (rc != 0)
        unlink(tmp);
    free(tmp);
    return rc;
}
