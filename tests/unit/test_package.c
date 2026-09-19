/* test_package.c — .gfpkg create/open/verify roundtrip + tamper detection. */
#include "checks.h"
#include "tests.h"
#include "../src/package.h"

#include <fcntl.h>
#include <sys/stat.h>


#include <sys/stat.h>


int test_package(void)
{
    char *dir = test_tmpdir("pkg");

    /* staging tree (DESTDIR-style) */
    char *stage = gf_path_join(dir, "stage");
    CHECK(gf_fs_mkdir_p(stage) == 0);
    test_wf(stage, "usr/local/bin/tool", "#!/bin/sh\necho tool\n");
    test_wf(stage, "usr/local/share/doc/tool/README", "docs for tool\n");
    {
        char *lx = gf_path_join(stage, "usr/local/bin/toolx");
        CHECK(gf_fs_symlink("tool", lx) == 0);
        free(lx);
    }

    gf_pkgmeta meta = { 0 };
    meta.name = gf_strdup("tool");
    meta.version = gf_strdup("1.2.3");
    meta.arch = gf_strdup("x86_64");
    meta.forge = gf_strdup("github");
    meta.repo_url = gf_strdup("https://github.com/owner/tool");
    meta.commit = gf_strdup("0123456789abcdef0123456789abcdef01234567");
    meta.source_status = gf_strdup("verified");
    meta.description = gf_strdup("test package");
    meta.license = gf_strdup("MIT");
    meta.toolchain_id = gf_strdup("cafebabe");
    meta.ndeps = 1;
    meta.dep_names = gf_malloc(sizeof(char *));
    meta.dep_specs = gf_malloc(sizeof(char *));
    meta.dep_kinds = gf_malloc(sizeof(char *));
    meta.dep_names[0] = gf_strdup("libz");
    meta.dep_specs[0] = gf_strdup(">=1.2");
    meta.dep_kinds[0] = gf_strdup("run");

    char *out = gf_path_join(dir, "tool-1.2.3-x86_64.gfpkg");
    gf_manifest_entry *manifest = NULL;
    size_t nents = 0;
    char *build_id = gf_package_create(stage, &meta, out, 0, &manifest,
                                       &nents);
    CHECK(build_id != NULL);
    CHECK(manifest != NULL);
    CHECK(nents >= 5); /* dirs + files + symlink */
    if (manifest) {
        bool saw_link = false, saw_file = false;
        for (size_t i = 0; i < nents; i++) {
            if (manifest[i].type == 'l')
                saw_link = true;
            if (manifest[i].type == 'f' && manifest[i].sha256)
                saw_file = true;
        }
        CHECK(saw_link && saw_file);
    }
    gf_manifest_free(manifest, nents);

    /* ---------------- determinism ---------------- */
    {
        /* perturb mtimes, rebuild: build id must be identical */
        struct timespec ts[2] = { {999, 0}, {999, 0} };
        char *tt = gf_path_join(stage, "usr/local/bin/tool");
        utimensat(AT_FDCWD, tt, ts, 0);
        free(tt);
        char *out2 = gf_path_join(dir, "tool2.gfpkg");
        char *bid2 = gf_package_create(stage, &meta, out2, 0, NULL, NULL);
        CHECK(bid2 != NULL);
        CHECK_STR(bid2, build_id);
        free(bid2);
        free(out2);
    }

    /* ---------------- open + metadata roundtrip ---------------- */
    gf_pkgmeta *m2 = NULL;
    gf_manifest_entry *ents = NULL;
    size_t ne = 0;
    char *bid_out = NULL;
    CHECK(gf_package_open(out, &m2, &ents, &ne, &bid_out) == 0);
    if (m2) {
        CHECK_STR(m2->name, "tool");
        CHECK_STR(m2->version, "1.2.3");
        CHECK_STR(m2->arch, "x86_64");
        CHECK_STR(m2->repo_url, "https://github.com/owner/tool");
        CHECK_STR(m2->commit, "0123456789abcdef0123456789abcdef01234567");
        CHECK_STR(m2->dep_names[0], "libz");
        CHECK_INT((long)m2->ndeps, 1);
        CHECK_STR(bid_out, build_id); /* recomputed hash matches create */
        /* build_id is the content address (sha of the file): it cannot be
         * embedded in METADATA itself, so it stays NULL there by design */
        CHECK(m2->build_id == NULL);
        gf_pkgmeta_free_heap(m2);
    }
    gf_manifest_free(ents, ne);
    free(bid_out);

    /* ---------------- verify (integrity) ---------------- */
    CHECK(gf_package_verify(out, NULL) == 0);

    /* ---------------- payload extraction ---------------- */
    char *pay = gf_path_join(dir, "payload");
    gf_fs_mkdir_p(pay);
    CHECK(gf_package_extract_payload(out, pay) == 0);
    char *toolpath = gf_path_join(pay, "usr/local/bin/tool");
    char *content = test_read(toolpath);
    CHECK_STR(content, "#!/bin/sh\necho tool\n");
    free(content);
    free(toolpath);
    {
        char *lx2 = gf_path_join(pay, "usr/local/bin/toolx");
        CHECK(gf_fs_is_symlink(lx2));
        free(lx2);
    }

    /* per-file hashing: pristine payload verifies */
    CHECK(gf_package_verify(out, pay) == 0);

    /* ---------------- tamper detection ---------------- */
    test_wf(pay, "usr/local/bin/tool", "#!/bin/sh\necho EVIL\n");
    CHECK(gf_package_verify(out, pay) == -1);

    /* structural verification catches a mutated archive */
    {
        /* flip a byte in the middle of the file */
        size_t flen = 0;
        char *buf = gf_fs_read_file(out, &flen);
        CHECK(buf != NULL && flen > 2048);
        if (buf && flen > 2048) {
            buf[flen / 2] = (char)(buf[flen / 2] ^ 0x5a);
            char *mut = gf_path_join(dir, "mutated.gfpkg");
            char *pd = gf_path_dirname(mut);
            gf_fs_mkdir_p(pd);
            free(pd);
            CHECK(gf_fs_write_file_atomic(mut, buf, flen) == 0);
            CHECK(gf_package_verify(mut, NULL) == -1);
            gf_fs_rm_rf(mut);
            free(mut);
        }
        free(buf);
    }

    gf_fs_rm_rf(dir);
    free(pay);
    free(out);
    free(stage);
    free(build_id);
    gf_pkgmeta_free(&meta);
    free(dir);
    return 0;
}
