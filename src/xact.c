/* xact.c — journaled filesystem transactions with backup/rollback. */
#include "xact.h"

#include "common.h"
#include "json.h"
#include "store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct step {
    gf_xact_op op;
    char *path;     /* absolute target path */
    char *src;      /* install source (store files path) */
    char *backup;   /* backup file path in txn dir (replace/remove) */
};

struct gf_xact {
    const gf_config *cfg;  /* borrowed */
    gf_db *db;             /* borrowed */
    char *id;
    char *dir;             /* <state>/transactions/<id> */
    char *journal;         /* dir/steps.jsonl */
    char *backup_dir;      /* dir/backup */
    struct step *steps;
    size_t nsteps;
    size_t cap;
    bool committed;
    bool rolled_back;
    FILE *journalf;
};

static void steps_free(struct step *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(s[i].path);
        free(s[i].src);
        free(s[i].backup);
    }
    free(s);
}

void gf_xact_free(gf_xact *x)
{
    if (!x)
        return;
    if (x->journalf)
        fclose(x->journalf);
    free(x->id);
    free(x->dir);
    free(x->journal);
    free(x->backup_dir);
    steps_free(x->steps, x->nsteps);
    free(x);
}

const char *gf_xact_id(const gf_xact *x) { return x->id; }
const char *gf_xact_dir(const gf_xact *x) { return x->dir; }

static char *new_txn_id(void)
{
    uint8_t rnd[8];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(rnd, 1, sizeof(rnd), f) != sizeof(rnd)) {
        for (size_t i = 0; i < sizeof(rnd); i++)
            rnd[i] = (uint8_t)(rand() >> 8);
    }
    if (f)
        fclose(f);
    char *id = gf_malloc(32);
    char hex[17];
    gf_hex_encode(hex, sizeof(hex), rnd, sizeof(rnd));
    hex[16] = '\0';
    snprintf(id, 32, "txn-%s", hex);
    return id;
}

static gf_xact *xact_create(const gf_config *cfg, gf_db *db, const char *id)
{
    gf_xact *x = gf_calloc(1, sizeof(gf_xact));
    x->cfg = cfg;
    x->db = db;
    x->id = gf_strdup(id);
    x->dir = gf_path_join_multi(cfg->state_dir, "transactions", id, NULL);
    x->journal = gf_path_join(x->dir, "steps.jsonl");
    x->backup_dir = gf_path_join(x->dir, "backup");
    return x;
}

gf_xact *gf_xact_begin(const gf_config *cfg, gf_db *db, const char *kind,
                       const char *pkg)
{
    char *id = new_txn_id();
    gf_xact *x = xact_create(cfg, db, id);
    free(id);
    if (gf_fs_mkdir_p(x->backup_dir) != 0) {
        gf_xact_free(x);
        return NULL;
    }
    if (gf_db_txn_begin(db, x->id, kind, pkg, x->journal) != 0) {
        gf_xact_free(x);
        return NULL;
    }
    x->journalf = fopen(x->journal, "a");
    if (!x->journalf) {
        gf_log_errno(GF_LOG_ERROR, x->journal);
        gf_xact_free(x);
        return NULL;
    }
    return x;
}

/* append a step record to the journal (line-buffered, fsync'd) */
static int journal_append(gf_xact *x, const struct step *s)
{
    if (!x->journalf) {
        gf_log(GF_LOG_ERROR,
               "xact: journal already closed (transaction %s was committed "
               "or rolled back); refusing further steps", x->id);
        return -1;
    }
    gf_json *o = gf_json_new_object();
    const char *opn;
    switch (s->op) {
    case GF_XACT_INSTALL:  opn = "install"; break;
    case GF_XACT_REPLACE:  opn = "replace"; break;
    case GF_XACT_REMOVE:   opn = "remove"; break;
    case GF_XACT_MKDIR:    opn = "mkdir"; break;
    case GF_XACT_RMDIR:    opn = "rmdir"; break;
    default:               opn = "?";
    }
    gf_json_object_set(o, "op", gf_json_new_string(opn));
    gf_json_object_set(o, "path", gf_json_new_string(s->path));
    if (s->src)
        gf_json_object_set(o, "src", gf_json_new_string(s->src));
    if (s->backup)
        gf_json_object_set(o, "backup", gf_json_new_string(s->backup));
    char *line = gf_json_dump(o);
    gf_json_free(o);
    if (!line)
        return -1;
    size_t len = strlen(line);
    if (fwrite(line, 1, len, x->journalf) != len ||
        fwrite("\n", 1, 1, x->journalf) != 1) {
        free(line);
        return -1;
    }
    free(line);
    fflush(x->journalf);
    int fd = fileno(x->journalf);
    fsync(fd);
    return 0;
}

static void record_step(gf_xact *x, struct step s);

/* Fault-injection hook for crash-recovery testing (docs/development.md):
 * GITFULL_XACT_CRASH_AFTER=<n> terminates the process immediately after
 * the n-th step has been applied AND journaled+fsynced, simulating a crash
 * mid-transaction. Inert unless the variable is set to a decimal number. */
static void fault_inject_crash(gf_xact *x);

static void record_step(gf_xact *x, struct step s)
{
    if (x->nsteps == x->cap) {
        x->cap = x->cap ? x->cap * 2 : 64;
        x->steps = gf_realloc(x->steps, x->cap * sizeof(struct step));
    }
    x->steps[x->nsteps++] = s;
    fault_inject_crash(x);
}

/* Fault-injection hook for crash-recovery testing (docs/development.md):
 * GITFULL_XACT_CRASH_AFTER=<n> terminates the process immediately after
 * the n-th step has been applied AND journaled+fsynced, simulating a crash
 * mid-transaction. Inert unless the variable is set to a decimal number. */
static void fault_inject_crash(gf_xact *x)
{
    const char *spec = getenv("GITFULL_XACT_CRASH_AFTER");
    if (!spec || !*spec)
        return;
    char *end = NULL;
    long n = strtol(spec, &end, 10);
    if (end == spec || *end != '\0' || n < 0)
        return;
    if ((long)x->nsteps == n) {
        if (x->journalf) {
            fflush(x->journalf);
            fsync(fileno(x->journalf));
        }
        _exit(107); /* no atexit handlers, no cleanup: hard crash */
    }
}

int gf_xact_check_conflict(gf_xact *x, const char *path, const char *pkg)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0; /* nothing there */
    char *owner = gf_db_file_owner(x->db, path);
    if (!owner) {
        gf_log(GF_LOG_ERROR,
               "file conflict: %s exists but is not owned by any gitfull "
               "package", path);
        return -1;
    }
    bool mine = strcmp(owner, pkg) == 0;
    free(owner);
    if (!mine) {
        gf_log(GF_LOG_ERROR,
               "file conflict: %s\nalready owned by another package "
               "(use `gitfull info <owner>`)", path);
        return -1;
    }
    return 0; /* owned by the same package: replace is fine */
}

int gf_xact_install_file(gf_xact *x, const char *src, const char *path,
                         const gf_manifest_entry *entry)
{
    (void)entry;
    struct stat st;
    bool exists = lstat(path, &st) == 0;

    /* backup existing (same-package previous version). The backup is made
     * BEFORE the step is journaled: an orphaned backup is harmless, while
     * a journaled REPLACE without a backup could not be undone. */
    char *backup = NULL;
    if (exists) {
        char *bbase = gf_path_basename(path);
        char *bname = gf_path_join(x->backup_dir, bbase);
        free(bbase);
        /* avoid collision in backup dir */
        size_t n = 1;
        while (gf_fs_exists(bname)) {
            char cand[512];
            snprintf(cand, sizeof(cand), "%s.%zu", bname, n++);
            free(bname);
            bname = gf_strdup(cand);
        }
        backup = bname;
        if (gf_fs_hardlink(path, backup) != 0) {
            /* hardlink may fail across devices or for symlinks: copy */
            if (S_ISLNK(st.st_mode)) {
                char *tgt = gf_fs_readlink(path);
                if (tgt) {
                    gf_fs_symlink(tgt, backup);
                    free(tgt);
                }
            } else if (gf_fs_copy_file(path, backup, (int)(st.st_mode & 0777)) != 0) {
                gf_log(GF_LOG_ERROR, "xact: backup failed for %s", path);
                free(backup);
                return -1;
            }
        }
    }

    /* WRITE-AHEAD: journal + fsync the step BEFORE mutating the filesystem.
     * A crash between the journal write and the mutation leaves a step that
     * undo tolerates (unlink/rmdir of a not-yet-created path), never an
     * orphan file the journal does not know about. */
    struct step s = { exists ? GF_XACT_REPLACE : GF_XACT_INSTALL,
                      gf_strdup(path), gf_strdup(src), backup };
    if (journal_append(x, &s) != 0) {
        free(s.path);
        free(s.src);
        free(s.backup);
        return -1;
    }
    record_step(x, s);

    if (exists && unlink(path) != 0 && errno != ENOENT) {
        gf_log_errno(GF_LOG_ERROR, "xact: unlink old");
        return -1;
    }

    /* create parent dirs (tracked only for top-level new dirs) */
    char *parent = gf_path_dirname(path);
    bool parent_existed = gf_fs_is_dir(parent);
    if (!parent_existed && gf_fs_mkdir_p(parent) != 0) {
        free(parent);
        return -1;
    }
    free(parent);

    /* install from store WITHOUT inode sharing: a reflink/copy keeps the
     * immutable store independent of in-place modification of the live
     * file (a hardlink here would let `verify`-detected tampering, or any
     * in-place patch by the admin, corrupt the store and every rollback
     * slot that shares the payload). */
    if (gf_fs_reflink_or_copy(src, path, entry ? (int)(entry->mode) : 0644) != 0) {
        return -1;
    }
    return 0;
}

int gf_xact_remove_file(gf_xact *x, const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0; /* already gone */
    /* Reserve the backup name and WRITE-AHEAD the step first: undo of a
     * REMOVE whose backup was never created simply leaves the original in
     * place (nothing is lost); a backup created without a journal entry
     * would orphan harmlessly inside the transaction dir. */
    char *backup = NULL;
    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        char *bbase = gf_path_basename(path);
        backup = gf_path_join(x->backup_dir, bbase);
        free(bbase);
        size_t n = 1;
        while (gf_fs_exists(backup)) {
            char cand[512];
            snprintf(cand, sizeof(cand), "%s.%zu", backup, n++);
            free(backup);
            backup = gf_strdup(cand);
        }
    }
    struct step s = { GF_XACT_REMOVE, gf_strdup(path), NULL, backup };
    if (journal_append(x, &s) != 0) {
        free(s.path);
        free(s.backup);
        return -1;
    }
    record_step(x, s);

    /* make the backup after the journal entry is durable */
    if (backup) {
        if (S_ISLNK(st.st_mode)) {
            char *tgt = gf_fs_readlink(path);
            if (tgt) {
                gf_fs_symlink(tgt, backup);
                free(tgt);
            }
        } else {
            gf_fs_hardlink(path, backup); /* best effort */
            if (!gf_fs_exists(backup))
                (void)gf_fs_copy_file(path, backup, (int)(st.st_mode & 0777));
            /* if the backup could not be created, the journaled path simply
             * does not exist and undo skips the restore (rare, same
             * best-effort semantics as before) */
        }
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        gf_log_errno(GF_LOG_ERROR, "xact: remove");
        return -1;
    }
    return 0;
}

int gf_xact_mkdir(gf_xact *x, const char *path)
{
    if (gf_fs_is_dir(path))
        return 0;
    /* write-ahead: journal before creating; undo tolerates ENOENT */
    struct step s = { GF_XACT_MKDIR, gf_strdup(path), NULL, NULL };
    if (journal_append(x, &s) != 0) {
        free(s.path);
        return -1;
    }
    record_step(x, s);
    if (gf_fs_mkdir_p(path) != 0)
        return -1;
    return 0;
}

int gf_xact_commit(gf_xact *x)
{
    if (x->committed)
        return 0;
    if (x->journalf) {
        fclose(x->journalf);
        x->journalf = NULL;
    }
    if (gf_db_txn_commit(x->db, x->id) != 0)
        return -1;
    x->committed = true;
    gf_fs_rm_rf(x->dir);
    return 0;
}

int gf_xact_rollback(gf_xact *x)
{
    if (x->rolled_back || x->committed)
        return 0;
    /* undo steps in reverse */
    for (size_t i = x->nsteps; i > 0; i--) {
        struct step *s = &x->steps[i - 1];
        switch (s->op) {
        case GF_XACT_INSTALL:
            unlink(s->path);
            break;
        case GF_XACT_REPLACE:
            unlink(s->path);
            if (s->backup && gf_fs_exists(s->backup)) {
                /* restore original */
                struct stat st;
                if (lstat(s->backup, &st) == 0 && S_ISLNK(st.st_mode)) {
                    char *tgt = gf_fs_readlink(s->backup);
                    if (tgt) {
                        gf_fs_symlink(tgt, s->path);
                        free(tgt);
                    }
                } else {
                    gf_fs_hardlink(s->backup, s->path);
                    if (!gf_fs_exists(s->path))
                        gf_fs_copy_file(s->backup, s->path, -1);
                }
            }
            break;
        case GF_XACT_REMOVE:
            if (s->backup && gf_fs_exists(s->backup)) {
                struct stat st;
                if (lstat(s->backup, &st) == 0 && S_ISLNK(st.st_mode)) {
                    char *tgt = gf_fs_readlink(s->backup);
                    if (tgt) {
                        gf_fs_symlink(tgt, s->path);
                        free(tgt);
                    }
                } else {
                    gf_fs_hardlink(s->backup, s->path);
                    if (!gf_fs_exists(s->path))
                        gf_fs_copy_file(s->backup, s->path, -1);
                }
            }
            break;
        case GF_XACT_MKDIR:
            gf_fs_rmdir_if_empty(s->path);
            break;
        case GF_XACT_RMDIR:
            gf_fs_mkdir_p(s->path);
            break;
        }
    }
    if (x->journalf) {
        fclose(x->journalf);
        x->journalf = NULL;
    }
    gf_db_txn_rollback(x->db, x->id);
    x->rolled_back = true;
    gf_fs_rm_rf(x->dir);
    return 0;
}

/* parse a journal file into steps (for recovery of interrupted txns) */
static int journal_load(const char *journal, struct step **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    size_t len = 0;
    char *text = gf_fs_read_file_limit(journal, 64 << 20, &len);
    if (!text)
        return 0; /* empty journal: nothing was applied */
    char *save = NULL;
    size_t cap = 32, count = 0;
    struct step *steps = gf_malloc(cap * sizeof(struct step));
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        gf_json *o = gf_json_parse(line, strlen(line), NULL, 0);
        if (!o || o->type != GF_JSON_OBJECT)
            continue;
        const char *op = gf_json_str(gf_json_get(o, "op"));
        const char *path = gf_json_str(gf_json_get(o, "path"));
        const char *src = gf_json_str(gf_json_get(o, "src"));
        const char *bak = gf_json_str(gf_json_get(o, "backup"));
        if (!op || !path)
            continue;
        struct step s = { 0 };
        if (strcmp(op, "install") == 0)
            s.op = GF_XACT_INSTALL;
        else if (strcmp(op, "replace") == 0)
            s.op = GF_XACT_REPLACE;
        else if (strcmp(op, "remove") == 0)
            s.op = GF_XACT_REMOVE;
        else if (strcmp(op, "mkdir") == 0)
            s.op = GF_XACT_MKDIR;
        else if (strcmp(op, "rmdir") == 0)
            s.op = GF_XACT_RMDIR;
        else
            continue;
        s.path = gf_strdup(path);
        s.src = src ? gf_strdup(src) : NULL;
        s.backup = bak ? gf_strdup(bak) : NULL;
        if (count == cap) {
            cap *= 2;
            steps = gf_realloc(steps, cap * sizeof(struct step));
        }
        steps[count++] = s;
        gf_json_free(o);
    }
    free(text);
    *out = steps;
    *n = count;
    return 0;
}

gf_xact *gf_xact_open_interrupted(const gf_config *cfg, gf_db *db,
                                  const char *id)
{
    gf_db_txn *txns = NULL;
    size_t n = 0;
    if (gf_db_txns(db, NULL, &txns, &n) != 0)
        return NULL;
    const gf_db_txn *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(txns[i].id, id) == 0) {
            found = &txns[i];
            break;
        }
    }
    if (!found) {
        gf_db_txns_free(txns, n);
        return NULL;
    }
    gf_xact *x = xact_create(cfg, db, id);
    /* note: dir paths must match the original journal's txn dir */
    free(x->dir);
    free(x->journal);
    free(x->backup_dir);
    x->dir = gf_strdup(found->journal ? found->journal : "");
    char *d = gf_path_dirname(x->dir);
    free(x->dir);
    x->dir = d;
    x->journal = gf_path_join(x->dir, "steps.jsonl");
    x->backup_dir = gf_path_join(x->dir, "backup");
    struct step *steps = NULL;
    size_t ns = 0;
    journal_load(x->journal, &steps, &ns);
    x->steps = steps;
    x->nsteps = ns;
    x->cap = ns;
    gf_db_txns_free(txns, n);
    return x;
}

int gf_xact_recover(const gf_config *cfg, gf_db *db, const char *id)
{
    gf_xact *x = gf_xact_open_interrupted(cfg, db, id);
    if (!x) {
        gf_log(GF_LOG_WARN, "transaction %s not found", id);
        return -1;
    }
    gf_log(GF_LOG_WARN, "recovering interrupted transaction %s (rolling back)",
           id);
    int rc = gf_xact_rollback(x);
    gf_xact_free(x);
    return rc;
}


