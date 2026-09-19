/* test_xact.c — journaled transactions: commit, conflict, rollback, crash
 * recovery (simulated via journal + interrupted DB txn on disk). */
#include "checks.h"
#include "tests.h"
#include "../src/config.h"
#include "../src/sha256.h"
#include "../src/db.h"
#include "../src/xact.h"

#include <fcntl.h>

int test_xact(void)
{
    char *root = test_tmpdir("xact");

    /* source file in a fake store */
    char *store = gf_path_join(root, "store-src");
    gf_fs_mkdir_p(store);
    test_wf(store, "file1", "new content 1");
    test_wf(store, "file2", "new content 2");

    gf_config *cfg = gf_config_load(root, NULL);
    CHECK(cfg != NULL);
    if (!cfg) {
        test_rmdir(root);
        free(store);
        return 0;
    }
    char *dbpath = gf_path_join(cfg->state_dir, "db/gitfull.db");
    {
        char *dbdir = gf_path_dirname(dbpath);
        gf_fs_mkdir_p(dbdir);
        free(dbdir);
    }
    gf_db *db = gf_db_open(dbpath);
    CHECK(db != NULL);
    char *s1path = gf_path_join(store, "file1");
    char *s2path = gf_path_join(store, "file2");

    char *target1 = gf_path_join(cfg->prefix, "bin/file1");
    char *target2 = gf_path_join(cfg->prefix, "bin/file2");

    /* ---------------- install + commit ---------------- */
    {
        gf_xact *x = gf_xact_begin(cfg, db, "install", "pkg");
        CHECK(x != NULL);
        if (x) {
            const char *id = gf_xact_id(x);
            CHECK(id != NULL && strlen(id) > 4);
            CHECK(gf_fs_is_dir(gf_xact_dir(x)));
            /* no conflict: path is new */
            CHECK(gf_xact_check_conflict(x, target1, "pkg") == 0);
            CHECK(gf_xact_install_file(x, s1path, target1, NULL) == 0);
            CHECK(gf_fs_is_file(target1));
            char *c = test_read(target1);
            CHECK_STR(c, "new content 1");
            free(c);
            CHECK(gf_xact_install_file(x, s2path, target2, NULL) == 0);
            CHECK(gf_xact_commit(x) == 0);
            CHECK(!gf_fs_exists(gf_xact_dir(x))); /* journal removed */
            /* ownership rows are written by the caller (ops.c); record them
             * here the same way the installer does, then verify lookup */
            char *h1 = gf_sha256_buf_hex("new content 1", 13);
            CHECK(gf_db_add_file(db, "pkg", target1, 'f', 0644, 13, h1,
                                 NULL) == 0);
            free(h1);
            char *owner = gf_db_file_owner(db, target1);
            CHECK_STR(owner, "pkg");
            free(owner);
            /* no unfinished transactions left */
            gf_db_txn *txns = NULL;
            size_t ntxn = 0;
            CHECK(gf_db_txns(db, NULL, &txns, &ntxn) == 0);
            CHECK_INT(ntxn, 0);
            gf_db_txns_free(txns, ntxn);
            gf_xact_free(x);
        }
    }

    /* ---------------- conflict detection ---------------- */
    {
        gf_xact *x = gf_xact_begin(cfg, db, "install", "pkg");
        CHECK(x != NULL);
        if (x) {
            /* existing unowned file blocks */
            char *unowned = gf_path_join(cfg->prefix, "share/unowned");
            test_write(unowned, "hands off");
            CHECK(gf_xact_check_conflict(x, unowned, "pkg") == -1);
            /* file owned by another package blocks */
            char *hother = gf_sha256_buf_hex("hands off", 9);
            CHECK(gf_db_add_file(db, "other", unowned, 'f', 0644, 9, hother,
                                 NULL) == 0);
            free(hother);
            CHECK(gf_xact_check_conflict(x, unowned, "pkg") == -1);
            CHECK(gf_xact_check_conflict(x, unowned, "other") == 0);
            free(unowned);
            /* own file is replaceable */
            CHECK(gf_xact_check_conflict(x, target1, "pkg") == 0);
            gf_xact_rollback(x);
            gf_xact_free(x);
        }
    }

    /* ---------------- replace + rollback restores original ---------------- */
    {
        /* register target1 as owned by pkg (previous version): the
         * installer refreshes ownership rows before an upgrade */
        char *orig = test_read(target1);
        CHECK_STR(orig, "new content 1");
        CHECK(gf_db_remove_files(db, "pkg") == 0);
        char *h = gf_sha256_buf_hex("new content 1", 13);
        CHECK(gf_db_add_file(db, "pkg", target1, 'f', 0644, 13, h, NULL) == 0);
        free(h);

        gf_xact *x = gf_xact_begin(cfg, db, "upgrade", "pkg");
        CHECK(x != NULL);
        if (x) {
            test_wf(store, "file1v2", "version two");
            char *f1v2 = gf_path_join(store, "file1v2");
            CHECK(gf_xact_install_file(x, f1v2, target1, NULL) == 0);
            free(f1v2);
            char *c = test_read(target1);
            CHECK_STR(c, "version two");
            free(c);
            CHECK(gf_xact_rollback(x) == 0);
            /* original restored byte-for-byte */
            char *r = test_read(target1);
            CHECK_STR(r, "new content 1");
            free(r);
            gf_xact_free(x);
        }
        /* a separate transaction removes a file; rollback restores it */
        gf_xact *xr = gf_xact_begin(cfg, db, "remove", "pkg");
        CHECK(xr != NULL);
        if (xr) {
            CHECK(gf_xact_remove_file(xr, target2) == 0);
            CHECK(!gf_fs_exists(target2));
            CHECK(gf_xact_rollback(xr) == 0);
            CHECK(gf_fs_is_file(target2));
            gf_xact_free(xr);
        }
        free(orig);
    }

    /* ---------------- crash recovery ---------------- */
    {
        /* crash: replace target1, journal step 1, then hard-abandon the
         * transaction (journal + db txn stay on disk, like a SIGKILL) */
        gf_xact *x = gf_xact_begin(cfg, db, "upgrade", "pkg");
        CHECK(x != NULL);
        char id[64];
        if (x) {
            snprintf(id, sizeof(id), "%s", gf_xact_id(x));
            test_wf(store, "file1v3", "version three");
            char *f1v3 = gf_path_join(store, "file1v3");
            CHECK(gf_xact_install_file(x, f1v3, target1, NULL) == 0);
            free(f1v3);
            /* pretend SIGKILL: free WITHOUT commit/rollback */
            gf_xact_free(x);
            /* the interrupted transaction is visible */
            gf_db_txn *txns = NULL;
            size_t ntxn = 0;
            CHECK(gf_db_txns(db, NULL, &txns, &ntxn) == 0);
            CHECK_INT(ntxn, 1);
            if (ntxn == 1)
                CHECK_STR(txns[0].id, id);
            gf_db_txns_free(txns, ntxn);
            /* recovery rolls it back */
            CHECK(gf_xact_recover(cfg, db, id) == 0);
            char *r = test_read(target1);
            CHECK_STR(r, "new content 1"); /* original survived the crash */
            free(r);
            /* db txn now closed */
            CHECK(gf_db_txns(db, NULL, &txns, &ntxn) == 0);
            CHECK_INT(ntxn, 0);
            gf_db_txns_free(txns, ntxn);
            /* journal dir cleaned */
            char *txroot = gf_path_join(cfg->state_dir, "transactions");
            char *tdir = gf_path_join(txroot, id);
            free(txroot);
            CHECK(!gf_fs_exists(tdir));
            free(tdir);
        }
    }

    gf_db_close(db);
    free(dbpath);
    free(s1path);
    free(s2path);
    free(target1);
    free(target2);
    free(store);
    gf_config_free(cfg);
    test_rmdir(root);
    return 0;
}
