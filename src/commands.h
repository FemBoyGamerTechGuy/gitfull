/* commands.h — command handlers (implemented in ops.c, clean.c, doctor.c). */
#ifndef GF_COMMANDS_H
#define GF_COMMANDS_H

#include "cli.h"

int cmd_install(gf_cmdctx *ctx, int argc, char **argv);
int cmd_remove(gf_cmdctx *ctx, int argc, char **argv);
int cmd_update(gf_cmdctx *ctx, int argc, char **argv);
int cmd_upgrade(gf_cmdctx *ctx, int argc, char **argv);
int cmd_search(gf_cmdctx *ctx, int argc, char **argv);
int cmd_info(gf_cmdctx *ctx, int argc, char **argv);
int cmd_list(gf_cmdctx *ctx, int argc, char **argv);
int cmd_rollback(gf_cmdctx *ctx, int argc, char **argv);
int cmd_hold(gf_cmdctx *ctx, int argc, char **argv);
int cmd_unhold(gf_cmdctx *ctx, int argc, char **argv);
int cmd_history(gf_cmdctx *ctx, int argc, char **argv);
int cmd_verify(gf_cmdctx *ctx, int argc, char **argv);
int cmd_doctor(gf_cmdctx *ctx, int argc, char **argv);
int cmd_clean(gf_cmdctx *ctx, int argc, char **argv);
int cmd_cache(gf_cmdctx *ctx, int argc, char **argv);
int cmd_build(gf_cmdctx *ctx, int argc, char **argv);
int cmd_repo(gf_cmdctx *ctx, int argc, char **argv);
int cmd_config(gf_cmdctx *ctx, int argc, char **argv);
int cmd_logs(gf_cmdctx *ctx, int argc, char **argv);

#endif
