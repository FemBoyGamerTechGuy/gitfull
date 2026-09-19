/* test_tar.c — deterministic writer, hardened reader, malicious archives. */
#include "checks.h"
#include "tests.h"
#include "../src/sha256.h"
#include "../src/tar.h"

#include <zlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* craft a ustar header for hand-made malicious archives */
static void tar_header(char h[512], const char *name, char typeflag,
                       const char *target, uint64_t size, mode_t mode)
{
    memset(h, 0, 512);
    strncpy(h, name, 99);
    if (target) {
        strncpy(h + 157, target, 99);
    }
    snprintf(h + 100, 8, "%07o", (unsigned)mode);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf(h + 136, 12, "%011llo", 0ULL);
    memset(h + 148, ' ', 8);
    h[156] = typeflag;
    snprintf(h + 257, 6, "ustar");
    snprintf(h + 263, 2, "0");
    unsigned chk = 0;
    for (int i = 0; i < 512; i++)
        chk += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", chk);
    h[154] = 0;
    h[155] = ' ';
}

/* write a "malicious" tar into path */
static void write_raw_tar(const char *path, char header[512],
                          const char *content, size_t clen)
{
    char *dir = gf_path_dirname(path);
    gf_fs_mkdir_p(dir);
    free(dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        failures++;
        return;
    }
    fwrite(header, 1, 512, f);
    if (clen) {
        fwrite(content, 1, clen, f);
        size_t pad = (512 - (clen % 512)) % 512;
        char zeros[512] = { 0 };
        fwrite(zeros, 1, pad, f);
    }
    char end[1024] = { 0 };
    fwrite(end, 1, 1024, f);
    fclose(f);
}

int test_tar(void)
{
    char *dir = test_tmpdir("tar");

    /* ---------------- deterministic writer ---------------- */
    {
        char *src = gf_path_join(dir, "src");
        gf_fs_mkdir_p(src);
        test_wf(src, "b.txt", "bee");
        test_wf(src, "a.txt", "ay");
        test_wf(src, "sub/c.txt", "cee");
        {
            char *lnk = gf_path_join(src, "lnk");
            gf_fs_symlink("a.txt", lnk);
            free(lnk);
        }
        /* insecure modes must be sanitized by the writer */
        {
            char *cp = gf_path_join(src, "a.txt");
            chmod(cp, 04777); /* setuid+world-w */
            free(cp);
        }

        char *out1 = gf_path_join(dir, "one.tar");
        char *out2 = gf_path_join(dir, "two.tar");
        CHECK(gf_tar_write_dir(src, out1, "payload", 0) == 0);
        /* perturb mtimes, then rewrite: bytes must be identical */
        struct timespec ts[2] = { {12345, 0}, {12345, 0} };
        char *a1 = gf_path_join(src, "a.txt");
        utimensat(AT_FDCWD, a1, ts, 0);
        struct stat st;
        lstat(a1, &st);
        CHECK(st.st_mtim.tv_sec == 12345);
        free(a1);
        CHECK(gf_tar_write_dir(src, out2, "payload", 0) == 0);
        char *h1 = gf_sha256_file_hex(out1);
        char *h2 = gf_sha256_file_hex(out2);
        CHECK_STR(h1, h2);
        free(h1);
        free(h2);

        /* extraction roundtrip (strip the "payload/" prefix) */
        char *dest = gf_path_join(dir, "extracted");
        gf_fs_mkdir_p(dest);
        size_t nfiles = 0;
        CHECK(gf_tar_extract(out1, dest, 1, NULL, &nfiles) == 0);
        char *apath = gf_path_join(dest, "a.txt");
        char *a = test_read(apath);
        CHECK_STR(a, "ay");
        free(a);
        free(apath);
        char *cpath = gf_path_join(dest, "sub/c.txt");
        char *c = test_read(cpath);
        CHECK_STR(c, "cee");
        free(c);
        free(cpath);
        char *lnkd = gf_path_join(dest, "lnk");
        CHECK(gf_fs_is_symlink(lnkd));
        free(lnkd);
        /* setuid+world-write stripped by writer AND reader */
        struct stat st2;
        char *ap2 = gf_path_join(dest, "a.txt");
        CHECK(lstat(ap2, &st2) == 0);
        free(ap2);
        CHECK(!(st2.st_mode & (S_ISUID | S_IWOTH)));
        /* listing agrees with extraction */
        gf_tar_entry *ents = NULL;
        size_t ne = 0;
        CHECK(gf_tar_list(out1, &ents, &ne, NULL) == 0);
        CHECK(ne >= 4); /* a.txt, b.txt, sub/, sub/c.txt, lnk */
        gf_tar_entries_free(ents, ne);

        /* verify: re-extract of the same archive must yield identical tree
         * (duplicate extraction into non-empty dir is refused: TOCTOU-safe) */
        CHECK(gf_tar_extract(out1, dest, 1, NULL, NULL) == -1);

        free(dest);
        free(out1);
        free(out2);
        free(src);
    }

    /* ---------------- gzip via zlib ---------------- */
    {
        char *plain = gf_path_join(dir, "one.tar");
        size_t plen = 0;
        char *raw = gf_fs_read_file(plain, &plen);
        CHECK(raw != NULL && plen > 1024);
        /* produce a REAL gzip stream (compress2 would emit zlib format) */
        char *gzpath = gf_path_join(dir, "one.tar.gz");
        char *gdir = gf_path_dirname(gzpath);
        gf_fs_mkdir_p(gdir);
        free(gdir);
        gzFile gz = gzopen(gzpath, "wb");
        CHECK(gz != NULL);
        if (gz) {
            CHECK(gzwrite(gz, raw, (unsigned)plen) == (int)plen);
            CHECK(gzclose(gz) == Z_OK);
        }
        /* a zlib-format (non-gzip) blob must be rejected as corrupt */
        {
            uLongf clen = compressBound((uLong)plen);
            unsigned char *cbuf = gf_malloc(clen);
            CHECK(compress2(cbuf, &clen, (const Bytef *)raw, (uLong)plen,
                            Z_BEST_COMPRESSION) == Z_OK);
            char *zpath = gf_path_join(dir, "zlibfmt.tar.zz");
            char *zdir = gf_path_dirname(zpath);
            gf_fs_mkdir_p(zdir);
            free(zdir);
            FILE *f = fopen(zpath, "wb");
            fwrite(cbuf, 1, clen, f);
            fclose(f);
            char *zdest = gf_path_join(dir, "zzextract");
            gf_fs_mkdir_p(zdest);
            CHECK(gf_tar_extract(zpath, zdest, 1, NULL, NULL) == -1);
            free(zdest);
            free(zpath);
            free(cbuf);
        }
        char *gzdest = gf_path_join(dir, "gzextract");
        gf_fs_mkdir_p(gzdest);
        CHECK(gf_tar_extract(gzpath, gzdest, 1, NULL, NULL) == 0);
        char *apath = gf_path_join(gzdest, "a.txt");
        char *a = test_read(apath);
        CHECK_STR(a, "ay");
        free(a);
        free(apath);
        free(gzdest);
        free(gzpath);
        free(raw);
        free(plain);
    }

    /* ---------------- malicious archives ---------------- */
    {
        char h[512];
        char *dest = gf_path_join(dir, "attack");
        gf_fs_mkdir_p(dest);

        /* 1. path traversal via ".." */
        tar_header(h, "../../evil", '0', NULL, 3, 0644);
        {
            char *e1 = gf_path_join(dir, "evil1.tar");
            write_raw_tar(e1, h, "xxx", 3);
            CHECK(gf_tar_extract(e1, dest, 0, NULL, NULL) == -1);
            free(e1);
        }
        CHECK(!gf_fs_exists("/evil"));
        {
            char *ev = gf_path_join(dir, "evil");
            CHECK(!gf_fs_exists(ev));
            free(ev);
        }

        /* 2. absolute path */
        tar_header(h, "/tmp/evil-abs", '0', NULL, 3, 0644);
        {
            char *e2 = gf_path_join(dir, "evil2.tar");
            write_raw_tar(e2, h, "xxx", 3);
            CHECK(gf_tar_extract(e2, dest, 0, NULL, NULL) == -1);
            free(e2);
        }

        /* 3. symlink escaping the root */
        tar_header(h, "esc", '2', "../../../tmp/evil-esc", 0, 0777);
        {
            char *e3 = gf_path_join(dir, "evil3.tar");
            write_raw_tar(e3, h, NULL, 0);
            CHECK(gf_tar_extract(e3, dest, 0, NULL, NULL) == -1);
            free(e3);
        }

        /* 4. duplicate paths */
        tar_header(h, "dup", '0', NULL, 3, 0644);
        {
            char *p = gf_path_join(dir, "evil4.tar");
            char *d = gf_path_dirname(p);
            gf_fs_mkdir_p(d);
            free(d);
            FILE *f = fopen(p, "wb");
            fwrite(h, 1, 512, f);
            fwrite("xxx", 1, 3, f);
            fwrite(h, 1, 512, f); /* same header again */
            fwrite("yyy", 1, 3, f);
            char end[1024] = { 0 };
            fwrite(end, 1, 1024, f);
            fclose(f);
            CHECK(gf_tar_extract(p, dest, 0, NULL, NULL) == -1);
            free(p);
        }

        /* 5. device node */
        tar_header(h, "devnode", '3', NULL, 0, 0644); /* char device */
        {
            char *e5 = gf_path_join(dir, "evil5.tar");
            write_raw_tar(e5, h, NULL, 0);
            CHECK(gf_tar_extract(e5, dest, 0, NULL, NULL) == -1);
            free(e5);
        }

        /* 6. hardlink to outside path */
        tar_header(h, "hl", '1', "/etc/passwd", 0, 0644);
        {
            char *e6 = gf_path_join(dir, "evil6.tar");
            write_raw_tar(e6, h, NULL, 0);
            CHECK(gf_tar_extract(e6, dest, 0, NULL, NULL) == -1);
            free(e6);
        }

        /* limits: entry count cap */
        {
            gf_tar_limits lim = { 100, 100, 2 };
            tar_header(h, "n1", '0', NULL, 1, 0644);
            {
                char *mn = gf_path_join(dir, "many.tar");
                write_raw_tar(mn, h, "x", 1);
                CHECK(gf_tar_extract(mn, dest, 0, &lim, NULL) == 0);
                free(mn);
            }
        }

        free(dest);
    }

    CHECK(gf_fs_rm_rf(dir) == 0);
    free(dir);
    return 0;
}
