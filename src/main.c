/* main.c — gitfull entry point: global options + command dispatch. */
#include "cli.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GF_VERSION "0.1.0"

int main(int argc, char **argv)
{
    const char *root = getenv("GITFULL_ROOT");
    const char *config_path = NULL;
    const char *state_dir = NULL;
    const char *prefix = NULL;
    bool yes = false, verbose = false, quiet = false;

    /* global options may appear before the command */
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--root") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(a, "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(a, "--state") == 0 && i + 1 < argc)
            state_dir = argv[++i];
        else if (strcmp(a, "--prefix") == 0 && i + 1 < argc)
            prefix = argv[++i];
        else if (strcmp(a, "--yes") == 0 || strcmp(a, "-y") == 0)
            yes = true;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0)
            verbose = true;
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0)
            quiet = true;
        else if (strcmp(a, "--version") == 0) {
            printf("gitfull %s\n", GF_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            gf_cli_usage();
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "gitfull: unknown option: %s\n", a);
            gf_cli_usage();
            return GF_EXIT_USAGE;
        } else {
            break; /* first non-option: the command */
        }
    }

    if (i >= argc) {
        gf_cli_usage();
        return GF_EXIT_USAGE;
    }

    const char *cmd_name = argv[i];
    const gf_cmd *cmd = gf_cli_find(cmd_name);
    if (!cmd) {
        /* allow global options after the command? no — unknown command */
        fprintf(stderr, "gitfull: unknown command: %s\n\n", cmd_name);
        gf_cli_usage();
        return GF_EXIT_USAGE;
    }

    /* load configuration */
    gf_log_set_level(quiet ? GF_LOG_WARN : (verbose ? GF_LOG_DEBUG : GF_LOG_INFO));
    gf_config *cfg = gf_config_load(root && *root ? root : NULL, config_path);
    if (!cfg)
        return GF_EXIT_ERROR;
    if (state_dir && *state_dir) {
        free(cfg->state_dir);
        cfg->state_dir = gf_strdup(state_dir);
    }
    if (prefix && *prefix) {
        free(cfg->prefix);
        cfg->prefix = gf_strdup(prefix);
    }
    if (gf_config_validate_runtime(cfg) != 0) {
        gf_config_free(cfg);
        return GF_EXIT_ERROR;
    }
    if (quiet) {
        free(cfg->log_level);
        cfg->log_level = gf_strdup("warn");
    }

    gf_cmdctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.yes = yes;
    ctx.verbose = verbose;
    ctx.quiet = quiet;

    /* command args: strip a --help at any position (handled per command) */
    int cmd_argc = argc - i - 1;
    char **cmd_argv = argv + i + 1;
    int rc = cmd->run(&ctx, cmd_argc, cmd_argv);
    gf_cli_ctx_free(&ctx);
    return rc;
}
