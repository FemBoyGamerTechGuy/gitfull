/* cli.h — command dispatch and shared command context. */
#ifndef GF_CLI_H
#define GF_CLI_H

#include "config.h"
#include "db.h"

#include <stdbool.h>

typedef struct gf_cmdctx {
    gf_config *cfg;
    gf_db *db;          /* opened lazily by commands that need it */
    bool yes;           /* --yes */
    bool verbose;
    bool quiet;
} gf_cmdctx;

/* command entry: name, short help, handler(argv after command), long help */
typedef struct gf_cmd {
    const char *name;
    const char *summary;
    int (*run)(gf_cmdctx *ctx, int argc, char **argv);
    const char *help;
} gf_cmd;

const gf_cmd *gf_cli_find(const char *name);
const gf_cmd *gf_cli_all(size_t *n);

/* open/close the DB on demand */
gf_db *gf_cli_db(gf_cmdctx *ctx);
void gf_cli_ctx_free(gf_cmdctx *ctx);

/* print the top-level help */
void gf_cli_usage(void);

/* open DB with schema; auto-init store dirs */
gf_db *gf_cli_db_open(const gf_config *cfg);

/* small helpers shared by commands */
int gf_cli_confirm(gf_cmdctx *ctx, const char *prompt);
char *gf_cli_state_db_path(const gf_config *cfg);

#endif /* GF_CLI_H */
