/* db.c — SQLite-backed transactional package database. */
#include "db.h"

#include "common.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gf_db {
    sqlite3 *h;
};

static const char SCHEMA[] =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=FULL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS meta("
    "  key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS packages("
    "  name TEXT PRIMARY KEY,"
    "  version TEXT NOT NULL,"
    "  build_id TEXT NOT NULL,"
    "  arch TEXT NOT NULL,"
    "  forge TEXT, repo_url TEXT, src_commit TEXT, source_sha TEXT,"
    "  pkg_sha TEXT NOT NULL,"
    "  build_identity TEXT,"
    "  toolchain_id TEXT,"
    "  source_status TEXT,"
    "  description TEXT,"
    "  license TEXT,"
    "  build_dir TEXT,"
    "  installed_at TEXT NOT NULL,"
    "  updated_at TEXT);"
    "CREATE TABLE IF NOT EXISTS pkg_files("
    "  package TEXT NOT NULL,"
    "  path TEXT NOT NULL,"
    "  type TEXT NOT NULL,"
    "  mode INTEGER NOT NULL,"
    "  size INTEGER NOT NULL,"
    "  sha256 TEXT,"
    "  target TEXT,"
    "  PRIMARY KEY(package, path),"
    "  UNIQUE(path));"
    "CREATE TABLE IF NOT EXISTS pkg_deps("
    "  package TEXT NOT NULL,"
    "  dep TEXT NOT NULL,"
    "  spec TEXT NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  PRIMARY KEY(package, dep, kind));"
    "CREATE TABLE IF NOT EXISTS history("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  package TEXT NOT NULL,"
    "  version TEXT,"
    "  event TEXT NOT NULL,"
    "  detail TEXT,"
    "  at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS rollback_slots("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  package TEXT NOT NULL,"
    "  version TEXT NOT NULL,"
    "  build_id TEXT NOT NULL,"
    "  activated_at TEXT,"
    "  deactivated_at TEXT,"
    "  active INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(package, version, build_id));"
    "CREATE TABLE IF NOT EXISTS holds("
    "  name TEXT PRIMARY KEY,"
    "  reason TEXT,"
    "  at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS transactions("
    "  id TEXT PRIMARY KEY,"
    "  kind TEXT NOT NULL,"
    "  package TEXT,"
    "  state TEXT NOT NULL,"
    "  journal TEXT NOT NULL,"
    "  started_at TEXT NOT NULL,"
    "  ended_at TEXT);"
    "PRAGMA user_version=1;";

gf_db *gf_db_open(const char *path)
{
    sqlite3 *h = NULL;
    char *dir = gf_path_dirname(path);
    if (dir && *dir && gf_fs_mkdir_p(dir) != 0) {
        free(dir);
        return NULL;
    }
    free(dir);
    if (sqlite3_open(path, &h) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: cannot open %s: %s", path,
               h ? sqlite3_errmsg(h) : "?");
        sqlite3_close(h);
        return NULL;
    }
    sqlite3_busy_timeout(h, 5000);
    char *err = NULL;
    if (sqlite3_exec(h, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: schema: %s", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(h);
        return NULL;
    }
    gf_db *db = gf_malloc(sizeof(gf_db));
    db->h = h;
    return db;
}

void gf_db_close(gf_db *db)
{
    if (!db)
        return;
    sqlite3_close(db->h);
    free(db);
}

int gf_db_begin(gf_db *db)
{
    char *err = NULL;
    if (sqlite3_exec(db->h, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: begin: %s", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int gf_db_commit(gf_db *db)
{
    char *err = NULL;
    if (sqlite3_exec(db->h, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: commit: %s", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int gf_db_rollback(gf_db *db)
{
    char *err = NULL;
    if (sqlite3_exec(db->h, "ROLLBACK", NULL, NULL, &err) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: rollback: %s", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- packages */

static const char *col_text(sqlite3_stmt *st, int col)
{
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? (const char *)t : NULL;
}

static char *col_strdup(sqlite3_stmt *st, int col)
{
    const char *t = col_text(st, col);
    return t ? gf_strdup(t) : NULL;
}

int gf_db_put_package(gf_db *db, const gf_pkgmeta *m, const char *build_dir)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT INTO packages(name,version,build_id,arch,forge,repo_url,"
            "src_commit,source_sha,pkg_sha,build_identity,toolchain_id,"
            "source_status,description,license,build_dir,installed_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(name) DO UPDATE SET version=excluded.version,"
            "build_id=excluded.build_id,arch=excluded.arch,"
            "forge=excluded.forge,repo_url=excluded.repo_url,"
            "src_commit=excluded.src_commit,source_sha=excluded.source_sha,"
            "pkg_sha=excluded.pkg_sha,build_identity=excluded.build_identity,"
            "toolchain_id=excluded.toolchain_id,"
            "source_status=excluded.source_status,"
            "description=excluded.description,license=excluded.license,"
            "build_dir=excluded.build_dir,updated_at=excluded.installed_at",
            -1, &st, NULL) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: prepare put_package: %s", sqlite3_errmsg(db->h));
        return -1;
    }
    sqlite3_bind_text(st, 1, m->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, m->version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, m->build_id ? m->build_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, m->arch, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, m->forge ? m->forge : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 6, m->repo_url ? m->repo_url : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 7, m->commit ? m->commit : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, m->source_sha ? m->source_sha : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, m->pkg_sha ? m->pkg_sha : (m->build_id ? m->build_id : ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, m->build_identity ? m->build_identity : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 11, m->toolchain_id ? m->toolchain_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 12, m->source_status ? m->source_status : "unsigned", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 13, m->description ? m->description : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 14, m->license ? m->license : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 15, build_dir ? build_dir : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 16, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        gf_log(GF_LOG_ERROR, "db: put_package: %s", sqlite3_errmsg(db->h));
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);

    /* deps */
    sqlite3_exec(db->h, "DELETE FROM pkg_deps WHERE package=?2", NULL, NULL, NULL);
    sqlite3_stmt *dd = NULL;
    if (sqlite3_prepare_v2(db->h,
            "DELETE FROM pkg_deps WHERE package=?", -1, &dd, NULL) == SQLITE_OK) {
        sqlite3_bind_text(dd, 1, m->name, -1, SQLITE_TRANSIENT);
        sqlite3_step(dd);
        sqlite3_finalize(dd);
    }
    for (size_t i = 0; i < m->ndeps; i++) {
        sqlite3_stmt *ds = NULL;
        if (sqlite3_prepare_v2(db->h,
                "INSERT OR REPLACE INTO pkg_deps(package,dep,spec,kind)"
                " VALUES(?,?,?,?)",
                -1, &ds, NULL) != SQLITE_OK)
            continue;
        sqlite3_bind_text(ds, 1, m->name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ds, 2, m->dep_names[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ds, 3, m->dep_specs[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ds, 4, m->dep_kinds[i], -1, SQLITE_TRANSIENT);
        sqlite3_step(ds);
        sqlite3_finalize(ds);
    }
    return 0;
}

gf_db_pkg *gf_db_get_package(gf_db *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "SELECT version,build_id,arch,forge,repo_url,src_commit,source_sha,"
            "pkg_sha,build_identity,toolchain_id,source_status,description,"
            "license,build_dir,installed_at,updated_at FROM packages"
            " WHERE name=?",
            -1, &st, NULL) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: get_package: %s", sqlite3_errmsg(db->h));
        return NULL;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return NULL;
    }
    gf_db_pkg *p = gf_calloc(1, sizeof(gf_db_pkg));
    gf_pkgmeta *m = &p->meta;
    m->name = gf_strdup(name);
    m->version = col_strdup(st, 0);
    m->build_id = col_strdup(st, 1);
    m->arch = col_strdup(st, 2);
    m->forge = col_strdup(st, 3);
    m->repo_url = col_strdup(st, 4);
    m->commit = col_strdup(st, 5);
    m->source_sha = col_strdup(st, 6);
    m->pkg_sha = col_strdup(st, 7);
    m->build_identity = col_strdup(st, 8);
    m->toolchain_id = col_strdup(st, 9);
    m->source_status = col_strdup(st, 10);
    m->description = col_strdup(st, 11);
    m->license = col_strdup(st, 12);
    p->build_dir = col_strdup(st, 13);
    p->installed_at = col_strdup(st, 14);
    p->updated_at = col_strdup(st, 15);
    sqlite3_finalize(st);

    /* deps */
    sqlite3_stmt *ds = NULL;
    if (sqlite3_prepare_v2(db->h,
            "SELECT dep,spec,kind FROM pkg_deps WHERE package=? ORDER BY dep",
            -1, &ds, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ds, 1, name, -1, SQLITE_TRANSIENT);
        size_t cap = 8;
        m->dep_names = gf_malloc(cap * sizeof(char *));
        m->dep_specs = gf_malloc(cap * sizeof(char *));
        m->dep_kinds = gf_malloc(cap * sizeof(char *));
        while (sqlite3_step(ds) == SQLITE_ROW) {
            if (m->ndeps == cap) {
                cap *= 2;
                m->dep_names = gf_realloc(m->dep_names, cap * sizeof(char *));
                m->dep_specs = gf_realloc(m->dep_specs, cap * sizeof(char *));
                m->dep_kinds = gf_realloc(m->dep_kinds, cap * sizeof(char *));
            }
            m->dep_names[m->ndeps] = col_strdup(ds, 0);
            m->dep_specs[m->ndeps] = col_strdup(ds, 1);
            m->dep_kinds[m->ndeps] = col_strdup(ds, 2);
            m->ndeps++;
        }
        sqlite3_finalize(ds);
    }
    return p;
}

void gf_db_pkg_free(gf_db_pkg *p)
{
    if (!p)
        return;
    gf_pkgmeta_free(&p->meta); /* fields only: meta is embedded */
    free(p->installed_at);
    free(p->updated_at);
    free(p->build_dir);
    free(p);
}

int gf_db_remove_package(gf_db *db, const char *name)
{
    static const char *tables[] = { "packages", "pkg_files", "pkg_deps", NULL };
    for (int i = 0; tables[i]; i++) {
        sqlite3_stmt *st = NULL;
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s=?",
                 tables[i], strcmp(tables[i], "packages") == 0 ? "name" : "package");
        if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK)
            continue;
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return 0;
}

char **gf_db_list_packages(gf_db *db, size_t *n)
{
    *n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT name FROM packages ORDER BY name",
                           -1, &st, NULL) != SQLITE_OK)
        return NULL;
    size_t cap = 16;
    char **out = gf_malloc(cap * sizeof(char *));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) {
            cap *= 2;
            out = gf_realloc(out, cap * sizeof(char *));
        }
        out[(*n)++] = col_strdup(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

size_t gf_db_count_packages(gf_db *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT COUNT(*) FROM packages", -1, &st,
                           NULL) != SQLITE_OK)
        return 0;
    size_t n = sqlite3_step(st) == SQLITE_ROW ? (size_t)sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

/* --------------------------------------------------------- file ownership */

int gf_db_add_file(gf_db *db, const char *pkg, const char *path, char type,
                   uint32_t mode, uint64_t size, const char *sha256,
                   const char *target)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT INTO pkg_files(package,path,type,mode,size,sha256,target)"
            " VALUES(?,?,?,?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK) {
        gf_log(GF_LOG_ERROR, "db: add_file prepare: %s", sqlite3_errmsg(db->h));
        return -1;
    }
    char t[2] = { type, 0 };
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, t, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, (int)mode);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)size);
    sqlite3_bind_text(st, 6, sha256 ? sha256 : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, target ? target : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        gf_log(GF_LOG_ERROR, "db: add_file %s: %s", path,
               sqlite3_errmsg(db->h));
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

int gf_db_remove_files(gf_db *db, const char *pkg)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "DELETE FROM pkg_files WHERE package=?", -1,
                           &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

char *gf_db_file_owner(gf_db *db, const char *path)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT package FROM pkg_files WHERE path=?",
                           -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    char *owner = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        owner = col_strdup(st, 0);
    sqlite3_finalize(st);
    return owner;
}

int gf_db_package_files(gf_db *db, const char *pkg, gf_manifest_entry **out,
                        size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "SELECT path,type,mode,size,sha256,target FROM pkg_files"
            " WHERE package=? ORDER BY path",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    size_t cap = 32, count = 0;
    gf_manifest_entry *list = gf_malloc(cap * sizeof(gf_manifest_entry));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count == cap) {
            cap *= 2;
            list = gf_realloc(list, cap * sizeof(gf_manifest_entry));
        }
        gf_manifest_entry *e = &list[count++];
        memset(e, 0, sizeof(*e));
        e->path = col_strdup(st, 0);
        const char *t = col_text(st, 1);
        e->type = t ? t[0] : 'f';
        e->mode = (uint32_t)sqlite3_column_int(st, 2);
        e->size = (uint64_t)sqlite3_column_int64(st, 3);
        e->sha256 = col_strdup(st, 4);
        e->target = col_strdup(st, 5);
    }
    sqlite3_finalize(st);
    *out = list;
    *n = count;
    return 0;
}

char **gf_db_reverse_deps(gf_db *db, const char *name, size_t *n)
{
    *n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "SELECT DISTINCT package FROM pkg_deps WHERE dep=? ORDER BY package",
            -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    size_t cap = 8;
    char **out = gf_malloc(cap * sizeof(char *));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) {
            cap *= 2;
            out = gf_realloc(out, cap * sizeof(char *));
        }
        out[(*n)++] = col_strdup(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

int gf_db_add_dep(gf_db *db, const char *pkg, const char *dep,
                  const char *spec, const char *kind)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT OR REPLACE INTO pkg_deps(package,dep,spec,kind)"
            " VALUES(?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, dep, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, spec, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, kind, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

/* ---------------------------------------------------------------- history */

int gf_db_add_history(gf_db *db, const char *pkg, const char *version,
                      const char *event, const char *detail)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT INTO history(package,version,event,detail,at)"
            " VALUES(?,?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, version ? version : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, event, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, detail ? detail : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, now, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int gf_db_history(gf_db *db, const char *pkg, gf_db_hist **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st = NULL;
    const char *sql = pkg
        ? "SELECT package,version,event,detail,at FROM history WHERE package=?"
          " ORDER BY id DESC"
        : "SELECT package,version,event,detail,at FROM history ORDER BY id DESC";
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (pkg)
        sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    size_t cap = 16, count = 0;
    gf_db_hist *list = gf_malloc(cap * sizeof(gf_db_hist));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count == cap) {
            cap *= 2;
            list = gf_realloc(list, cap * sizeof(gf_db_hist));
        }
        gf_db_hist *h = &list[count++];
        h->package = col_strdup(st, 0);
        h->version = col_strdup(st, 1);
        h->event = col_strdup(st, 2);
        h->detail = col_strdup(st, 3);
        h->at = col_strdup(st, 4);
    }
    sqlite3_finalize(st);
    *out = list;
    *n = count;
    return 0;
}

void gf_db_hist_free(gf_db_hist *h, size_t n)
{
    if (!h)
        return;
    for (size_t i = 0; i < n; i++) {
        free(h[i].package);
        free(h[i].version);
        free(h[i].event);
        free(h[i].detail);
        free(h[i].at);
    }
    free(h);
}

/* -------------------------------------------------------- rollback slots */

int gf_db_add_slot(gf_db *db, const char *pkg, const char *version,
                   const char *build_id, bool active)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT OR IGNORE INTO rollback_slots(package,version,build_id,"
            "activated_at,active) VALUES(?,?,?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, build_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, active ? now : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, active ? 1 : 0);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (active) {
        /* ensure only one active slot per package */
        sqlite3_stmt *u = NULL;
        if (sqlite3_prepare_v2(db->h,
                "UPDATE rollback_slots SET active=0, deactivated_at=?"
                " WHERE package=? AND NOT (version=? AND build_id=?)",
                -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_text(u, 1, now, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(u, 2, pkg, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(u, 3, version, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(u, 4, build_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    return 0;
}

int gf_db_deactivate_slots(gf_db *db, const char *pkg)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *u = NULL;
    if (sqlite3_prepare_v2(db->h,
            "UPDATE rollback_slots SET active=0, deactivated_at=? WHERE package=?",
            -1, &u, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(u, 1, now, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(u, 2, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_step(u);
    sqlite3_finalize(u);
    return 0;
}

int gf_db_slots(gf_db *db, const char *pkg, gf_db_slot **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "SELECT package,version,build_id,activated_at,deactivated_at,active"
            " FROM rollback_slots WHERE package=? ORDER BY rowid DESC",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    size_t cap = 8, count = 0;
    gf_db_slot *list = gf_malloc(cap * sizeof(gf_db_slot));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count == cap) {
            cap *= 2;
            list = gf_realloc(list, cap * sizeof(gf_db_slot));
        }
        gf_db_slot *s = &list[count++];
        memset(s, 0, sizeof(*s));
        s->package = col_strdup(st, 0);
        s->version = col_strdup(st, 1);
        s->build_id = col_strdup(st, 2);
        s->activated_at = col_strdup(st, 3);
        s->deactivated_at = col_strdup(st, 4);
        s->active = sqlite3_column_int(st, 5) != 0;
    }
    sqlite3_finalize(st);
    *out = list;
    *n = count;
    return 0;
}

void gf_db_slots_free(gf_db_slot *s, size_t n)
{
    if (!s)
        return;
    for (size_t i = 0; i < n; i++) {
        free(s[i].package);
        free(s[i].version);
        free(s[i].build_id);
        free(s[i].activated_at);
        free(s[i].deactivated_at);
    }
    free(s);
}

int gf_db_drop_slot(gf_db *db, const char *pkg, const char *version,
                    const char *build_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "DELETE FROM rollback_slots WHERE package=? AND version=? AND build_id=?",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, build_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

gf_db_slot *gf_db_find_slot(gf_db *db, const char *pkg, const char *version)
{
    gf_db_slot *slots = NULL;
    size_t n = 0;
    if (gf_db_slots(db, pkg, &slots, &n) != 0)
        return NULL;
    gf_db_slot *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(slots[i].version, version) == 0) {
            found = gf_memdup(&slots[i], sizeof(gf_db_slot));
            /* re-own strings */
            gf_db_slot *f = found;
            f->package = gf_strdup(slots[i].package);
            f->version = gf_strdup(slots[i].version);
            f->build_id = gf_strdup(slots[i].build_id);
            f->activated_at = gf_strdup(slots[i].activated_at ? "" : "");
            if (slots[i].activated_at) {
                free(f->activated_at);
                f->activated_at = gf_strdup(slots[i].activated_at);
            }
            if (slots[i].deactivated_at) {
                f->deactivated_at = gf_strdup(slots[i].deactivated_at);
            } else {
                f->deactivated_at = NULL;
            }
            break;
        }
    }
    gf_db_slots_free(slots, n);
    return found;
}

/* ------------------------------------------------------------------ holds */

int gf_db_hold(gf_db *db, const char *pkg, const char *reason)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT OR REPLACE INTO holds(name,reason,at) VALUES(?,?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, reason ? reason : "manual", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, now, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int gf_db_unhold(gf_db *db, const char *pkg)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "DELETE FROM holds WHERE name=?", -1, &st,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

bool gf_db_is_held(gf_db *db, const char *pkg)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT 1 FROM holds WHERE name=?", -1, &st,
                           NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, pkg, -1, SQLITE_TRANSIENT);
    bool held = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return held;
}

char **gf_db_holds(gf_db *db, size_t *n)
{
    *n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT name FROM holds ORDER BY name", -1,
                           &st, NULL) != SQLITE_OK)
        return NULL;
    size_t cap = 8;
    char **out = gf_malloc(cap * sizeof(char *));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (*n == cap) {
            cap *= 2;
            out = gf_realloc(out, cap * sizeof(char *));
        }
        out[(*n)++] = col_strdup(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}

/* ----------------------------------------------------------- transactions */

int gf_db_txn_begin(gf_db *db, const char *id, const char *kind,
                    const char *pkg, const char *journal_path)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT INTO transactions(id,kind,package,state,journal,started_at)"
            " VALUES(?,?,?,'active',?,?)"
            " ON CONFLICT(id) DO UPDATE SET state='active',"
            " journal=excluded.journal, started_at=excluded.started_at",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, pkg ? pkg : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, journal_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, now, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int gf_db_txn_commit(gf_db *db, const char *id)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "UPDATE transactions SET state='committed', ended_at=? WHERE id=?",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, now, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int gf_db_txn_rollback(gf_db *db, const char *id)
{
    char now[24];
    gf_time_iso8601_now(now, sizeof(now));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "UPDATE transactions SET state='rolled_back', ended_at=? WHERE id=?",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, now, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

int gf_db_txns(gf_db *db, const char *state, gf_db_txn **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    sqlite3_stmt *st = NULL;
    const char *sql = state
        ? "SELECT id,kind,package,state,journal,started_at,ended_at FROM"
          " transactions WHERE state=? ORDER BY started_at DESC"
        : "SELECT id,kind,package,state,journal,started_at,ended_at FROM"
          " transactions WHERE state NOT IN ('committed','rolled_back')"
          " ORDER BY started_at DESC";
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (state)
        sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
    size_t cap = 8, count = 0;
    gf_db_txn *list = gf_malloc(cap * sizeof(gf_db_txn));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (count == cap) {
            cap *= 2;
            list = gf_realloc(list, cap * sizeof(gf_db_txn));
        }
        gf_db_txn *t = &list[count++];
        memset(t, 0, sizeof(*t));
        t->id = col_strdup(st, 0);
        t->kind = col_strdup(st, 1);
        t->package = col_strdup(st, 2);
        t->state = col_strdup(st, 3);
        t->journal = col_strdup(st, 4);
        t->started_at = col_strdup(st, 5);
        t->ended_at = col_strdup(st, 6);
    }
    sqlite3_finalize(st);
    *out = list;
    *n = count;
    return 0;
}

void gf_db_txns_free(gf_db_txn *t, size_t n)
{
    if (!t)
        return;
    for (size_t i = 0; i < n; i++) {
        free(t[i].id);
        free(t[i].kind);
        free(t[i].package);
        free(t[i].state);
        free(t[i].journal);
        free(t[i].started_at);
        free(t[i].ended_at);
    }
    free(t);
}

/* -------------------------------------------------------------------- meta */

int gf_db_meta_set(gf_db *db, const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h,
            "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 0;
}

char *gf_db_meta_get(gf_db *db, const char *key)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "SELECT value FROM meta WHERE key=?", -1,
                           &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    char *v = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = col_strdup(st, 0);
    sqlite3_finalize(st);
    return v;
}

int gf_db_integrity(gf_db *db)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, "PRAGMA integrity_check", -1, &st, NULL) != SQLITE_OK)
        return -1;
    int rc = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *r = col_text(st, 0);
        rc = r && strcmp(r, "ok") == 0 ? 0 : -1;
    }
    sqlite3_finalize(st);
    return rc;
}
