/* clean.c — safe garbage collection. */
#include "clean.h"
#include "cli.h"

#include "common.h"
#include "db.h"
#include "store.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GC rules:
 *  - build dirs: always removable (they are scratch)
 *  - source/git/tarball caches: removable
 *  - store versions: removable ONLY when
 *      * not the active version, AND
 *      * not within the rollback retention window, AND
 *      * not referenced as a dependency by an installed package, AND
 *      * not the only payload of a held package's history? (holds protect
 *        active versions; rollback slots of held packages keep retention)
 */
int gf_clean_run(gf_cmdctx *ctx, int argc, char **argv)
{
    bool dry = false, all = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry = true;
        else if (strcmp(argv[i], "--all") == 0)
            all = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("clean")->help);
            return 0;
        }
    }
    gf_config *cfg = ctx->cfg;
    gf_db *db = gf_cli_db(ctx);

    /* 1. build dirs */
    {
        char *bdir = gf_path_join(cfg->state_dir, "builds");
        uint64_t bytes = 0;
        gf_fs_du(bdir, &bytes);
        printf("build directories: %llu bytes%s\n", (unsigned long long)bytes,
               dry ? " (dry run)" : "");
        if (!dry)
            gf_fs_rm_rf(bdir);
        free(bdir);
    }

    /* 2. caches */
    {
        char *c1 = gf_path_join(cfg->state_dir, "cache/tarballs");
        char *c2 = gf_path_join(cfg->state_dir, "cache/git");
        uint64_t bytes = 0, b2 = 0;
        gf_fs_du(c1, &bytes);
        gf_fs_du(c2, &b2);
        bytes += b2;
        printf("caches:            %llu bytes%s\n", (unsigned long long)bytes,
               dry ? " (dry run)" : "");
        if (!dry) {
            gf_fs_rm_rf(c1);
            gf_fs_rm_rf(c2);
        }
        free(c1);
        free(c2);
    }

    /* 3. store GC (only with --all) */
    if (all && db) {
        size_t n = 0;
        char **pkgs = gf_db_list_packages(db, &n);
        uint64_t freed = 0;
        for (size_t i = 0; i < n; i++) {
            gf_db_pkg *p = gf_db_get_package(db, pkgs[i]);
            if (!p)
                continue;
            /* reverse deps protect the whole package's versions */
            size_t nrdep = 0;
            char **rdeps = gf_db_reverse_deps(db, pkgs[i], &nrdep);
            bool required = nrdep > 0;
            gf_strv_free(rdeps, nrdep);
            if (required) {
                gf_db_pkg_free(p);
                continue;
            }
            /* collect versions in the store for this package */
            size_t nver = 0;
            char **versions = gf_store_versions(cfg->state_dir, pkgs[i], &nver);
            (void)versions;
            /* slots retained */
            gf_db_slot *slots = NULL;
            size_t nslots = 0;
            gf_db_slots(db, pkgs[i], &slots, &nslots);
            /* count deactivated slots from newest to oldest, keep
             * cfg->rollback_hold of them */
            size_t kept = 0;
            for (size_t s = 0; s < nslots; s++) {
                if (slots[s].active)
                    continue; /* active: never GC */
                if (kept < (size_t)cfg->rollback_hold) {
                    kept++;
                    continue;
                }
                /* beyond retention: delete unless referenced */
                char *dir = gf_path_join_multi(cfg->state_dir, "store",
                                               pkgs[i], slots[s].version, NULL);
                uint64_t bytes = 0;
                gf_fs_du(dir, &bytes);
                printf("drop rollback version %s %s%s (%llu bytes)\n", pkgs[i],
                       slots[s].version, dry ? " (dry run)" : "",
                       (unsigned long long)bytes);
                freed += bytes;
                if (!dry) {
                    gf_store_remove_version(cfg->state_dir, pkgs[i],
                                            slots[s].version, slots[s].build_id);
                    gf_db_drop_slot(db, pkgs[i], slots[s].version,
                                    slots[s].build_id);
                }
                free(dir);
            }
            gf_db_slots_free(slots, nslots);
            gf_db_pkg_free(p);
        }
        gf_strv_free(pkgs, n);
        printf("store GC:          %llu bytes reclaimable%s\n",
               (unsigned long long)freed, dry ? " (dry run)" : "");
    }

    (void)gf_store_versions;
    return 0;
}
