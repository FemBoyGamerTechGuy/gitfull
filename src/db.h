/* db.h — transactional package database (SQLite, WAL mode).
 *
 * Tables: meta, packages, pkg_files (UNIQUE(path) = file ownership),
 * pkg_deps, history, rollback_slots, holds, transactions.
 */
#ifndef GF_DB_H
#define GF_DB_H

#include "package.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gf_db gf_db;

/* Open (creating when needed) the database at path. Returns NULL on error. */
gf_db *gf_db_open(const char *path);
void gf_db_close(gf_db *db);

/* transactions: BEGIN IMMEDIATE / COMMIT / ROLLBACK */
int gf_db_begin(gf_db *db);
int gf_db_commit(gf_db *db);
int gf_db_rollback(gf_db *db);

/* ---------------- packages ---------------- */
typedef struct gf_db_pkg {
    gf_pkgmeta meta;    /* name/version/arch/... + deps */
    char *installed_at; /* ISO-8601 */
    char *updated_at;
    char *build_dir;    /* logs location for this build */
} gf_db_pkg;

int gf_db_put_package(gf_db *db, const gf_pkgmeta *m, const char *build_dir);
gf_db_pkg *gf_db_get_package(gf_db *db, const char *name);
int gf_db_remove_package(gf_db *db, const char *name);
size_t gf_db_count_packages(gf_db *db);
/* iterate: returns names (owned array of malloc'd strings) */
char **gf_db_list_packages(gf_db *db, size_t *n);
/* does the recorded build_dir still have logs? helper for `logs` */
void gf_db_pkg_free(gf_db_pkg *p);

/* ---------------- file ownership ---------------- */
int gf_db_add_file(gf_db *db, const char *pkg, const char *path, char type,
                   uint32_t mode, uint64_t size, const char *sha256,
                   const char *target);
int gf_db_remove_files(gf_db *db, const char *pkg);
/* who owns this path? NULL when unowned. malloc'd package name. */
char *gf_db_file_owner(gf_db *db, const char *path);
/* list files of a package */
int gf_db_package_files(gf_db *db, const char *pkg, gf_manifest_entry **out,
                        size_t *n);

/* ---------------- dependencies ---------------- */
int gf_db_add_dep(gf_db *db, const char *pkg, const char *dep,
                  const char *spec, const char *kind);
/* reverse deps: packages that depend on 'name' */
char **gf_db_reverse_deps(gf_db *db, const char *name, size_t *n);

/* ---------------- history ---------------- */
int gf_db_add_history(gf_db *db, const char *pkg, const char *version,
                      const char *event, const char *detail);
typedef struct gf_db_hist {
    char *package;
    char *version;
    char *event;
    char *detail;
    char *at;
} gf_db_hist;
int gf_db_history(gf_db *db, const char *pkg, gf_db_hist **out, size_t *n);
void gf_db_hist_free(gf_db_hist *h, size_t n);

/* ---------------- rollback slots ---------------- */
typedef struct gf_db_slot {
    char *package;
    char *version;
    char *build_id;
    char *activated_at;
    char *deactivated_at;
    bool active;
} gf_db_slot;
int gf_db_add_slot(gf_db *db, const char *pkg, const char *version,
                   const char *build_id, bool active);
/* deactivate the currently active slot of a package */
int gf_db_deactivate_slots(gf_db *db, const char *pkg);
int gf_db_slots(gf_db *db, const char *pkg, gf_db_slot **out, size_t *n);
void gf_db_slots_free(gf_db_slot *s, size_t n);
/* drop a specific slot row (payload deleted by caller) */
int gf_db_drop_slot(gf_db *db, const char *pkg, const char *version,
                    const char *build_id);
/* slot for a specific version */
gf_db_slot *gf_db_find_slot(gf_db *db, const char *pkg, const char *version);

/* ---------------- holds ---------------- */
int gf_db_hold(gf_db *db, const char *pkg, const char *reason);
int gf_db_unhold(gf_db *db, const char *pkg);
bool gf_db_is_held(gf_db *db, const char *pkg);
char **gf_db_holds(gf_db *db, size_t *n);

/* ---------------- transactions (crash recovery journal) ---------------- */
int gf_db_txn_begin(gf_db *db, const char *id, const char *kind,
                    const char *pkg, const char *journal_path);
int gf_db_txn_commit(gf_db *db, const char *id);
int gf_db_txn_rollback(gf_db *db, const char *id);
typedef struct gf_db_txn {
    char *id;
    char *kind;
    char *package;
    char *state;
    char *journal;
    char *started_at;
    char *ended_at;
} gf_db_txn;
/* list transactions in a given state (NULL = any except committed) */
int gf_db_txns(gf_db *db, const char *state, gf_db_txn **out, size_t *n);
void gf_db_txns_free(gf_db_txn *t, size_t n);

/* ---------------- meta key/value ---------------- */
int gf_db_meta_set(gf_db *db, const char *key, const char *value);
char *gf_db_meta_get(gf_db *db, const char *key);

/* integrity check: 0 when ok */
int gf_db_integrity(gf_db *db);

#endif /* GF_DB_H */
