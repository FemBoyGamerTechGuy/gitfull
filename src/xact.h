/* xact.h — journaled filesystem transactions for atomic activation.
 *
 * Model: every filesystem mutation of an activation (install file,
 * replace old-version file, remove old file, mkdir) is journaled BEFORE it
 * is applied. Originals are backed up via hardlinks in a transaction backup
 * directory. On failure or crash the journal is replayed backwards to
 * restore the previous state; `gitfull doctor` detects interrupted
 * transactions and offers/ performs recovery.
 *
 * Layout:
 *   <state>/transactions/<id>/
 *     steps.jsonl   applied steps (append + fsync per step)
 *     backup/       hardlinked originals of replaced/removed files
 *
 * The database row for the transaction commits LAST (after all fs steps
 * are durable). Crash windows are always recoverable to "previous state".
 */
#ifndef GF_XACT_H
#define GF_XACT_H

#include "config.h"
#include "db.h"
#include "package.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_xact gf_xact;

typedef enum {
    GF_XACT_INSTALL = 0,   /* install (hardlink) src -> path */
    GF_XACT_REPLACE,       /* replace existing file (backed up first) */
    GF_XACT_REMOVE,        /* remove file (backed up first) */
    GF_XACT_MKDIR,         /* directory created */
    GF_XACT_RMDIR          /* directory removed (empty) */
} gf_xact_op;

/* Open a new transaction. kind: install|remove|rollback|upgrade. */
gf_xact *gf_xact_begin(const gf_config *cfg, gf_db *db, const char *kind,
                       const char *pkg);
/* Find and open an interrupted (active) transaction for recovery. */
gf_xact *gf_xact_open_interrupted(const gf_config *cfg, gf_db *db,
                                  const char *id);
void gf_xact_free(gf_xact *x);

const char *gf_xact_id(const gf_xact *x);
const char *gf_xact_dir(const gf_xact *x);

/* Pre-flight conflict check: path exists on disk and is not owned by
 * 'pkg'. Returns 0 (ok), -1 with conflict logged. */
int gf_xact_check_conflict(gf_xact *x, const char *path, const char *pkg);

/* Plan + apply steps. Each returns 0/-1 (logged). All steps journal+fsync. */
int gf_xact_install_file(gf_xact *x, const char *src, const char *path,
                         const gf_manifest_entry *entry);
/* remove a file owned by a (previous version of a) package; backed up */
int gf_xact_remove_file(gf_xact *x, const char *path);
int gf_xact_mkdir(gf_xact *x, const char *path);

/* Commit: DB txn was already used by caller; marks journal committed,
 * records history + rollback slot, removes the transaction directory. */
int gf_xact_commit(gf_xact *x);

/* Rollback: undo all applied steps (restore backups, remove installed
 * files). Marks the DB transaction rolled back. */
int gf_xact_rollback(gf_xact *x);

/* Recover an interrupted transaction (roll it back). Used by doctor. */
int gf_xact_recover(const gf_config *cfg, gf_db *db, const char *id);

#endif /* GF_XACT_H */
