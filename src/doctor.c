/* doctor.c — `gitfull doctor`: deep diagnostics. */
#include "cli.h"
#include "commands.h"
#include "doctor.h"

#include "build.h"
#include "cgroup.h"
#include "common.h"
#include "forge.h"
#include "gitx.h"
#include "http.h"
#include "sandbox.h"
#include "seccomp.h"
#include "toolchain.h"
#include "store.h"
#include "xact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

typedef struct diag {
    int pass;
    int warn;
    int fail;
} diag;

static void check(diag *d, const char *label, int ok, bool warn_only,
                  const char *detail)
{
    if (ok > 0) {
        printf("  [ok]   %s%s%s\n", label, detail ? ": " : "", detail ? detail : "");
        d->pass++;
    } else if (warn_only) {
        printf("  [warn] %s%s%s\n", label, detail ? ": " : "", detail ? detail : "");
        d->warn++;
    } else {
        printf("  [FAIL] %s%s%s\n", label, detail ? ": " : "", detail ? detail : "");
        d->fail++;
    }
}

int gf_doctor_run(gf_cmdctx *ctx, int argc, char **argv)
{
    bool online = getenv("GITFULL_ONLINE") && *getenv("GITFULL_ONLINE");
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--online") == 0)
            online = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: gitfull doctor [--online]\n"
                   "  --online  also probe forge API connectivity (network)\n");
            return 0;
        }
    }
    diag d = { 0, 0, 0 };
    gf_config *cfg = ctx->cfg;

    printf("gitfull doctor\n=============\n");

    /* 1. configuration */
    printf("configuration:\n");
    check(&d, "config parses", 1, false, cfg->path ? cfg->path : "built-in defaults");
    check(&d, "config validates", gf_config_validate(cfg) == 0, false, NULL);
    check(&d, "state dir writable",
          gf_fs_mkdir_p(cfg->state_dir) == 0 && access(cfg->state_dir, W_OK) == 0,
          false, cfg->state_dir);
    char *dbpath = gf_cli_state_db_path(cfg);
    struct stat st;
    bool db_exists = stat(dbpath, &st) == 0;
    gf_db *db = gf_cli_db(ctx);
    check(&d, "package database", db != NULL, false,
          db ? (db_exists ? dbpath : "created") : dbpath);

    /* 2. database integrity */
    if (db) {
        check(&d, "database integrity (PRAGMA integrity_check)",
              gf_db_integrity(db) == 0, false, NULL);
        size_t npkgs = gf_db_count_packages(db);
        char detail[64];
        snprintf(detail, sizeof(detail), "%zu package(s)", npkgs);
        check(&d, "packages tracked", 1, false, detail);
        /* interrupted transactions */
        gf_db_txn *txns = NULL;
        size_t ntxn = 0;
        gf_db_txns(db, NULL, &txns, &ntxn);
        check(&d, "no interrupted transactions", ntxn == 0, false,
              ntxn == 0 ? NULL : "run `gitfull doctor` recovery below");
        for (size_t i = 0; i < ntxn; i++) {
            printf("        recovering %s (%s %s)\n", txns[i].id, txns[i].kind,
                   txns[i].package ? txns[i].package : "");
            gf_xact_recover(cfg, db, txns[i].id);
        }
        gf_db_txns_free(txns, ntxn);
        /* rollback consistency: every active slot's payload exists */
        size_t n = 0;
        char **pkgs = gf_db_list_packages(db, &n);
        int inconsistent = 0;
        for (size_t i = 0; i < n; i++) {
            gf_db_pkg *p = gf_db_get_package(db, pkgs[i]);
            if (!p)
                continue;
            char *pp = gf_store_pkg_path(cfg->state_dir, pkgs[i],
                                         p->meta.version, p->meta.build_id);
            if (!gf_fs_is_file(pp)) {
                printf("        [FAIL] store payload missing for %s %s\n",
                       pkgs[i], p->meta.version);
                inconsistent++;
            }
            free(pp);
            /* dependency consistency */
            for (size_t k = 0; k < p->meta.ndeps; k++) {
                gf_db_pkg *dep = gf_db_get_package(db, p->meta.dep_names[k]);
                if (!dep) {
                    printf("        [warn] dependency %s of %s is not installed\n",
                           p->meta.dep_names[k], pkgs[i]);
                    d.warn++;
                } else {
                    gf_db_pkg_free(dep);
                }
            }
            gf_db_pkg_free(p);
        }
        gf_strv_free(pkgs, n);
        check(&d, "store/db consistency", inconsistent == 0, false, NULL);
    }

    /* 3. sandbox + toolchain */
    printf("build environment:\n");
    bool userns = gf_sandbox_userns_available();
    check(&d, "user namespaces (unprivileged sandboxes)", userns, true,
          userns ? "available" : "unavailable — builds fall back to degraded isolation");
    check(&d, "seccomp syscall filter", gf_seccomp_supported(), false,
          gf_seccomp_describe());
    check(&d, "cgroup v2 resource limits", gf_cgroup_available(), true,
          gf_cgroup_available() ? "unified hierarchy mounted"
                                : "not delegated (memory/cpu limits skipped; "
                                  "common in containers — not an error)");
    char gitver[64] = "";
    check(&d, "git", gf_git_check(gitver, sizeof(gitver)) == 0, false, gitver);
    char curlver[64] = "";
    check(&d, "curl (TLS)", gf_http_toolcheck(curlver, sizeof(curlver)) == 0,
          false, curlver);
    {
        gf_toolchain tc;
        if (gf_toolchain_probe(cfg, "make", &tc) == 0) {
            char detail[256];
            const gf_tool *cc = gf_toolchain_get(&tc, "cc");
            snprintf(detail, sizeof(detail), "%s (%s)",
                     cc && cc->version ? cc->version : "not found",
                     cc && cc->sha256 ? "identity recorded" : "missing");
            check(&d, "bootstrap toolchain (cc)", cc != NULL, true, detail);
            const gf_tool *mk = gf_toolchain_get(&tc, "make");
            snprintf(detail, sizeof(detail), "%s",
                     mk && mk->version ? mk->version : "not found");
            check(&d, "make", mk != NULL, true, detail);
            gf_toolchain_free(&tc);
        } else {
            check(&d, "toolchain probe", 0, false, NULL);
        }
    }

    /* 4. forge connectivity (opt-in: doctor stays offline by default) */
    if (online) {
    printf("forge connectivity:\n");
    for (size_t r = 0;; r++) {
        const gf_repo *repo = gf_config_repo_enabled(ctx->cfg, r);
        if (!repo)
            break;
        gf_forge *f = gf_forge_open(repo, cfg);
        if (!f)
            continue;
        char *default_branch = NULL, *desc = NULL, *lic = NULL;
        int rc = f->ops->repo_info(f, "gitfull", "gitfull", &default_branch,
                                   &desc, &lic);
        /* 404 is fine: API reachable. -1 means network/auth trouble. */
        bool reachable = rc >= 0 && rc != -1;
        if (rc == 1)
            reachable = true; /* 404 -> API works */
        free(default_branch);
        free(desc);
        free(lic);
        char detail[256];
        snprintf(detail, sizeof(detail), "%s (%s)", repo->api_url,
                 f->token ? "authenticated" : "anonymous");
        check(&d, repo->name, reachable, true, detail);
        gf_forge_close(f);
    }
    } /* end if (online) */

    /* 5. filesystem */
    printf("filesystem:\n");
    {
        struct statvfs vfs;
        char *sd = gf_strdup(cfg->state_dir);
        if (statvfs(sd, &vfs) == 0) {
            uint64_t free_mb = (uint64_t)vfs.f_bavail * vfs.f_frsize / (1024 * 1024);
            char detail[64];
            snprintf(detail, sizeof(detail), "%llu MiB free on %s",
                     (unsigned long long)free_mb, sd);
            check(&d, "disk space", free_mb > 512, true, detail);
        }
        free(sd);
        uint64_t usage = gf_store_usage(cfg->state_dir);
        char detail[64];
        snprintf(detail, sizeof(detail), "%llu bytes",
                 (unsigned long long)usage);
        check(&d, "store usage", 1, false, detail);
    }

    printf("----\n%d passed, %d warnings, %d failures\n", d.pass, d.warn,
           d.fail);
    return d.fail > 0 ? GF_EXIT_ERROR : 0;
}
