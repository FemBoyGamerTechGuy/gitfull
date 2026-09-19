/* cli.c — command table, help text, DB bootstrap. */
#include "cli.h"

#include "commands.h"
#include "common.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static const gf_cmd CMDS[] = {
    { "install", "install a package (stable release by default)",
      cmd_install,
      "usage: gitfull install <package> [options]\n"
      "  package: owner/repository | github:owner/repo | codeberg:owner/repo |\n"
      "           gitlab:owner/repo | git+https://... | https://host/owner/repo |\n"
      "           local:/path/to/repo | owner/repo@v1.2.3\n"
      "options:\n"
      "  --edge           install the branch head instead of the stable release\n"
      "  --prerelease     allow prerelease tags when picking the latest release\n"
      "  --skip-tests     do not run the package's test suite\n"
      "  --force          rebuild even when the build-id exists\n"
      "  --dry-run        resolve and show the plan without building\n"
      "  --yes            do not ask for confirmation\n" },
    { "remove", "remove an installed package",
      cmd_remove,
      "usage: gitfull remove <package> [--yes]\n"
      "  removes the package's files (atomically, with rollback journal)\n" },
    { "update", "refresh metadata and show available upgrades",
      cmd_update,
      "usage: gitfull update [--apply]\n"
      "  refreshes forge metadata, checks installed packages against the\n"
      "  newest stable releases and reports what would be upgraded.\n"
      "  --apply  perform the upgrades (same as `gitfull upgrade`)\n" },
    { "upgrade", "upgrade installed packages (respects holds)",
      cmd_upgrade,
      "usage: gitfull upgrade [--yes] [--dry-run]\n" },
    { "search", "search repositories on enabled forges",
      cmd_search,
      "usage: gitfull search <query>\n" },
    { "info", "show package information (installed or remote)",
      cmd_info,
      "usage: gitfull info <package>\n" },
    { "list", "list installed packages",
      cmd_list,
      "usage: gitfull list\n" },
    { "rollback", "restore a previous package version",
      cmd_rollback,
      "usage: gitfull rollback <package> [--to <version>] [--yes]\n"
      "  without --to, rolls back to the most recent retained version\n" },
    { "hold", "hold packages (hold/unhold/list subcommands)",
      cmd_hold,
      "usage: gitfull hold <package>\n"
      "       gitfull unhold <package>\n"
      "       gitfull hold list\n"
      "  held packages are skipped by `gitfull update`/`upgrade`\n" },
    { "unhold", "remove a hold",
      cmd_unhold,
      "usage: gitfull unhold <package>\n" },
    { "history", "show install/upgrade/rollback history",
      cmd_history,
      "usage: gitfull history [package]\n" },
    { "verify", "verify installed files against the database",
      cmd_verify,
      "usage: gitfull verify [package|--all]\n" },
    { "doctor", "diagnose the gitfull installation",
      cmd_doctor,
      "usage: gitfull doctor\n"
      "  checks config, database, store, sandbox, toolchain, network,\n"
      "  interrupted transactions, rollback consistency\n" },
    { "clean", "garbage-collect build dirs, caches, old versions",
      cmd_clean,
      "usage: gitfull clean [--dry-run] [--all]\n"
      "  removes build directories and source/package caches; with --all\n"
      "  also drops rollback versions beyond the retention setting\n"
      "  (never touches active, held, or dependency-required versions)\n" },
    { "cache", "inspect the cache",
      cmd_cache,
      "usage: gitfull cache [clean|stats]\n" },
    { "build", "build a package without installing it",
      cmd_build,
      "usage: gitfull build <package> [--edge] [--skip-tests] [--force]\n" },
    { "repo", "manage repositories (list/add/remove/enable/disable)",
      cmd_repo,
      "usage: gitfull repo list\n"
      "       gitfull repo add <name> <kind> <api-url> [--enable]\n"
      "       gitfull repo remove <name>\n"
      "       gitfull repo enable|disable <name>\n" },
    { "config", "show/validate configuration",
      cmd_config,
      "usage: gitfull config show|validate|path\n" },
    { "logs", "show build logs for a package",
      cmd_logs,
      "usage: gitfull logs <package>\n" },
    { NULL, NULL, NULL, NULL }
};

const gf_cmd *gf_cli_find(const char *name)
{
    for (int i = 0; CMDS[i].name; i++) {
        if (strcmp(CMDS[i].name, name) == 0)
            return &CMDS[i];
    }
    return NULL;
}

const gf_cmd *gf_cli_all(size_t *n)
{
    size_t c = 0;
    while (CMDS[c].name)
        c++;
    *n = c;
    return CMDS;
}

void gf_cli_usage(void)
{
    printf("gitfull 0.1.0 — source/forge-based Linux package manager\n\n"
           "usage: gitfull [options] <command> [arguments]\n\n"
           "options:\n"
           "  --root DIR     use DIR as an alternative system root (testing)\n"
           "  --config PATH  configuration file (default /etc/gitfull.conf)\n"
           "  --state DIR    state directory override (default /var/lib/gitfull)\n"
           "  --prefix DIR   install prefix override (default /usr/local)\n"
           "  --yes          assume yes for confirmations\n"
           "  -v, --verbose  debug logging\n"
           "  -q, --quiet    warnings only\n"
           "  -h, --help     this help\n"
           "  --version      version information\n\n"
           "commands:\n");
    for (int i = 0; CMDS[i].name; i++)
        printf("  %-10s %s\n", CMDS[i].name, CMDS[i].summary);
    printf("\nsee `gitfull <command> --help` for command details.\n"
           "documentation: docs/ in the gitfull repository.\n");
}

char *gf_cli_state_db_path(const gf_config *cfg)
{
    return gf_path_join(cfg->state_dir, "db/gitfull.db");
}

gf_db *gf_cli_db_open(const gf_config *cfg)
{
    if (gf_fs_mkdir_p_soft(cfg->state_dir) != 0) {
        gf_log(GF_LOG_WARN,
               "state directory %s is not writable; package queries fall "
               "back to remote-only (installs need --state or permissions)",
               cfg->state_dir);
        return NULL;
    }
    char *dbp = gf_cli_state_db_path(cfg);
    gf_db *db = gf_db_open(dbp);
    free(dbp);
    if (!db)
        return NULL;
    gf_db_meta_set(db, "state_dir", cfg->state_dir);
    gf_db_meta_set(db, "prefix", cfg->prefix);
    return db;
}

gf_db *gf_cli_db(gf_cmdctx *ctx)
{
    if (ctx->db)
        return ctx->db;
    ctx->db = gf_cli_db_open(ctx->cfg);
    return ctx->db;
}

void gf_cli_ctx_free(gf_cmdctx *ctx)
{
    gf_db_close(ctx->db);
    gf_config_free(ctx->cfg);
    memset(ctx, 0, sizeof(*ctx));
}

int gf_cli_confirm(gf_cmdctx *ctx, const char *prompt)
{
    if (ctx->yes)
        return 1;
    if (!isatty(0))
        return 0; /* non-interactive: refuse by default */
    fprintf(stderr, "%s [y/N] ", prompt);
    fflush(stderr);
    char buf[16] = { 0 };
    if (!fgets(buf, sizeof(buf), stdin))
        return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}
