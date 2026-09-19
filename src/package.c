/* package.c — .gfpkg creation, parsing, verification, payload extraction. */
#include "package.h"

#include "common.h"
#include "sha256.h"
#include "tar.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------- metadata */

/* Frees the owned FIELDS of m; the container itself is NOT freed (m may be
 * embedded in a larger struct or stack-allocated). */
void gf_pkgmeta_free(gf_pkgmeta *m)
{
    if (!m)
        return;
    free(m->name);
    free(m->version);
    free(m->arch);
    free(m->forge);
    free(m->repo_url);
    free(m->commit);
    free(m->source_sha);
    free(m->source_status);
    free(m->description);
    free(m->license);
    free(m->build_id);
    free(m->pkg_sha);
    free(m->build_identity);
    free(m->toolchain_id);
    for (size_t i = 0; i < m->ndeps; i++) {
        free(m->dep_names[i]);
        free(m->dep_specs[i]);
        free(m->dep_kinds[i]);
    }
    free(m->dep_names);
    free(m->dep_specs);
    free(m->dep_kinds);
    memset(m, 0, sizeof(*m));
}

/* Frees fields AND a heap-allocated container. */
void gf_pkgmeta_free_heap(gf_pkgmeta *m)
{
    if (!m)
        return;
    gf_pkgmeta_free(m);
    free(m);
}

gf_pkgmeta *gf_pkgmeta_copy(const gf_pkgmeta *m)
{
    if (!m)
        return NULL;
    gf_pkgmeta *c = gf_memdup(m, sizeof(*m));
    c->name = gf_strdup(m->name ? m->name : "");
    c->version = gf_strdup(m->version ? m->version : "");
    c->arch = gf_strdup(m->arch ? m->arch : "");
    c->forge = m->forge ? gf_strdup(m->forge) : NULL;
    c->repo_url = m->repo_url ? gf_strdup(m->repo_url) : NULL;
    c->commit = m->commit ? gf_strdup(m->commit) : NULL;
    c->source_sha = m->source_sha ? gf_strdup(m->source_sha) : NULL;
    c->source_status = m->source_status ? gf_strdup(m->source_status) : NULL;
    c->description = m->description ? gf_strdup(m->description) : NULL;
    c->license = m->license ? gf_strdup(m->license) : NULL;
    c->build_id = m->build_id ? gf_strdup(m->build_id) : NULL;
    c->pkg_sha = m->pkg_sha ? gf_strdup(m->pkg_sha) : NULL;
    c->build_identity = m->build_identity ? gf_strdup(m->build_identity) : NULL;
    c->toolchain_id = m->toolchain_id ? gf_strdup(m->toolchain_id) : NULL;
    c->dep_names = gf_malloc((m->ndeps ? m->ndeps : 1) * sizeof(char *));
    c->dep_specs = gf_malloc((m->ndeps ? m->ndeps : 1) * sizeof(char *));
    c->dep_kinds = gf_malloc((m->ndeps ? m->ndeps : 1) * sizeof(char *));
    c->ndeps = m->ndeps;
    for (size_t i = 0; i < m->ndeps; i++) {
        c->dep_names[i] = gf_strdup(m->dep_names[i]);
        c->dep_specs[i] = gf_strdup(m->dep_specs[i]);
        c->dep_kinds[i] = gf_strdup(m->dep_kinds[i]);
    }
    return c;
}

gf_json *gf_pkgmeta_to_json(const gf_pkgmeta *m)
{
    gf_json *o = gf_json_new_object();
    gf_json_object_set(o, "format", gf_json_new_string("gfpkg-1"));
    gf_json_object_set(o, "name", gf_json_new_string(m->name));
    gf_json_object_set(o, "version", gf_json_new_string(m->version));
    gf_json_object_set(o, "architecture", gf_json_new_string(m->arch));
    if (m->forge)
        gf_json_object_set(o, "forge", gf_json_new_string(m->forge));
    if (m->repo_url)
        gf_json_object_set(o, "repository", gf_json_new_string(m->repo_url));
    if (m->commit)
        gf_json_object_set(o, "commit", gf_json_new_string(m->commit));
    if (m->source_sha)
        gf_json_object_set(o, "source_sha256", gf_json_new_string(m->source_sha));
    if (m->source_status)
        gf_json_object_set(o, "source_status", gf_json_new_string(m->source_status));
    if (m->description)
        gf_json_object_set(o, "description", gf_json_new_string(m->description));
    if (m->license)
        gf_json_object_set(o, "license", gf_json_new_string(m->license));
    if (m->toolchain_id)
        gf_json_object_set(o, "toolchain_id", gf_json_new_string(m->toolchain_id));
    if (m->build_identity) {
        /* embed build identity as parsed JSON (canonical on dump) */
        gf_json *bi = gf_json_parse(m->build_identity, strlen(m->build_identity),
                                    NULL, 0);
        if (bi)
            gf_json_object_set(o, "build_identity", bi);
        else
            gf_json_object_set(o, "build_identity", gf_json_new_string(m->build_identity));
    }
    if (m->pkg_sha)
        gf_json_object_set(o, "package_sha256", gf_json_new_string(m->pkg_sha));
    gf_json *deps = gf_json_new_array();
    for (size_t i = 0; i < m->ndeps; i++) {
        gf_json *d = gf_json_new_object();
        gf_json_object_set(d, "name", gf_json_new_string(m->dep_names[i]));
        gf_json_object_set(d, "spec", gf_json_new_string(m->dep_specs[i]));
        gf_json_object_set(d, "kind", gf_json_new_string(m->dep_kinds[i]));
        gf_json_array_push(deps, d);
    }
    gf_json_object_set(o, "dependencies", deps);
    return o;
}

gf_pkgmeta *gf_pkgmeta_from_json(const gf_json *obj)
{
    if (!obj || obj->type != GF_JSON_OBJECT)
        return NULL;
    gf_pkgmeta *m = gf_calloc(1, sizeof(*m));
    m->name = gf_strdup(gf_json_str(gf_json_get(obj, "name")));
    m->version = gf_strdup(gf_json_str(gf_json_get(obj, "version")));
    m->arch = gf_strdup(gf_json_str(gf_json_get(obj, "architecture")));
    if (!m->name || !m->version || !m->arch)
        goto fail;
    m->forge = gf_strdup(gf_json_str(gf_json_get(obj, "forge")));
    m->repo_url = gf_strdup(gf_json_str(gf_json_get(obj, "repository")));
    m->commit = gf_strdup(gf_json_str(gf_json_get(obj, "commit")));
    m->source_sha = gf_strdup(gf_json_str(gf_json_get(obj, "source_sha256")));
    m->source_status = gf_strdup(gf_json_str(gf_json_get(obj, "source_status")));
    m->description = gf_strdup(gf_json_str(gf_json_get(obj, "description")));
    m->license = gf_strdup(gf_json_str(gf_json_get(obj, "license")));
    m->toolchain_id = gf_strdup(gf_json_str(gf_json_get(obj, "toolchain_id")));
    m->pkg_sha = gf_strdup(gf_json_str(gf_json_get(obj, "package_sha256")));
    const gf_json *bi = gf_json_get(obj, "build_identity");
    if (bi) {
        if (bi->type == GF_JSON_STRING)
            m->build_identity = gf_strdup(bi->v.str);
        else {
            char *dump = gf_json_dump(bi);
            m->build_identity = dump;
        }
    }
    const gf_json *deps = gf_json_get(obj, "dependencies");
    if (deps && deps->type == GF_JSON_ARRAY) {
        size_t n = gf_json_len(deps);
        m->dep_names = gf_malloc((n ? n : 1) * sizeof(char *));
        m->dep_specs = gf_malloc((n ? n : 1) * sizeof(char *));
        m->dep_kinds = gf_malloc((n ? n : 1) * sizeof(char *));
        for (size_t i = 0; i < n; i++) {
            const gf_json *d = gf_json_at(deps, i);
            if (!d || d->type != GF_JSON_OBJECT)
                continue;
            const char *nm = gf_json_str(gf_json_get(d, "name"));
            if (!nm)
                continue;
            m->dep_names[m->ndeps] = gf_strdup(nm);
            m->dep_specs[m->ndeps] = gf_strdup(
                gf_json_str(gf_json_get(d, "spec")) ? gf_json_str(gf_json_get(d, "spec")) : "*");
            m->dep_kinds[m->ndeps] = gf_strdup(
                gf_json_str(gf_json_get(d, "kind")) ? gf_json_str(gf_json_get(d, "kind")) : "run");
            m->ndeps++;
        }
    }
    if (!m->dep_names) {
        m->dep_names = gf_malloc(sizeof(char *));
        m->dep_specs = gf_malloc(sizeof(char *));
        m->dep_kinds = gf_malloc(sizeof(char *));
    }
    return m;
fail:
    gf_pkgmeta_free_heap(m);
    return NULL;
}

void gf_manifest_free(gf_manifest_entry *entries, size_t n)
{
    if (!entries)
        return;
    for (size_t i = 0; i < n; i++) {
        free(entries[i].path);
        free(entries[i].sha256);
        free(entries[i].target);
    }
    free(entries);
}

/* ------------------------------------------------------------ staging scan */

static int scan_cmp(const void *a, const void *b)
{
    const gf_manifest_entry *ea = a, *eb = b;
    return strcmp(ea->path, eb->path);
}

static uint32_t sanitize(uint32_t m, bool is_dir)
{
    m &= 0777;
    m &= ~(uint32_t)(S_ISUID | S_ISGID | S_IWOTH);
    if (is_dir)
        m |= 0700;
    return m;
}

/* Walk the staging dir and produce manifest entries (sorted). */
static int scan_staging(const char *dir, gf_manifest_entry **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    size_t cap = 0, count = 0;
    gf_manifest_entry *list = NULL;

    /* recursive walk */
    struct walk_ud {
        gf_manifest_entry **list;
        size_t *count;
        size_t *cap;
    } ud = { &list, &count, &cap };

    (void)ud;

    /* iterative DFS with sorted children */
    typedef struct frame {
        char *dir;
    } frame;
    (void)sizeof(frame);

    /* simpler: recursive helper */
    struct ctx2 {
        gf_manifest_entry **list;
        size_t *count;
        size_t *cap;
        int fail;
    } ctx = { &list, &count, &cap, 0 };

    /* forward via nested function not allowed; do it manually below */
    (void)ctx;

    /* collect all paths recursively with lstat */
    gf_strbuf paths = { 0 };
    (void)paths;

    size_t np = 0;
    char **all = NULL;
    /* recursive collect function implemented via stack */
    enum { MAXD = 4096 };
    char **stack = gf_malloc(MAXD * sizeof(char *));
    size_t sp = 0;
    stack[sp++] = gf_strdup(dir);
    cap = 64;
    list = gf_malloc(cap * sizeof(gf_manifest_entry));
    while (sp > 0) {
        char *cur = stack[--sp];
        size_t nentries = 0;
        char **entries = NULL;
        if (gf_fs_list_dir(cur, &entries, &nentries) != 0) {
            free(cur);
            goto fail;
        }
        /* sort children for determinism */
        for (size_t i = 0; i + 1 < nentries; i++) {
            for (size_t k = i + 1; k < nentries; k++) {
                if (strcmp(entries[i], entries[k]) > 0) {
                    char *t = entries[i];
                    entries[i] = entries[k];
                    entries[k] = t;
                }
            }
        }
        for (size_t i = 0; i < nentries; i++) {
            struct stat st;
            if (lstat(entries[i], &st) != 0) {
                gf_strv_free(entries, nentries);
                free(cur);
                goto fail;
            }
            const char *rel = entries[i] + strlen(dir);
            if (*rel == '/')
                rel++;
            if (count == cap) {
                cap *= 2;
                list = gf_realloc(list, cap * sizeof(gf_manifest_entry));
            }
            gf_manifest_entry *e = &list[count++];
            memset(e, 0, sizeof(*e));
            e->path = gf_strdup(rel);
            if (S_ISDIR(st.st_mode)) {
                e->type = 'd';
                e->mode = sanitize((uint32_t)st.st_mode, true);
                e->size = 0;
                if (sp < MAXD)
                    stack[sp++] = gf_strdup(entries[i]);
            } else if (S_ISREG(st.st_mode)) {
                e->type = 'f';
                e->mode = sanitize((uint32_t)st.st_mode, false);
                e->size = (uint64_t)st.st_size;
                e->sha256 = gf_sha256_file_hex(entries[i]);
                if (!e->sha256) {
                    gf_strv_free(entries, nentries);
                    free(cur);
                    goto fail;
                }
            } else if (S_ISLNK(st.st_mode)) {
                e->type = 'l';
                e->mode = 0777;
                e->size = 0;
                e->target = gf_fs_readlink(entries[i]);
                if (!e->target) {
                    gf_strv_free(entries, nentries);
                    free(cur);
                    goto fail;
                }
            } else {
                gf_log(GF_LOG_WARN, "package: skipping special file: %s",
                       entries[i]);
            }
            free(entries[i]);
        }
        free(entries);
        free(cur);
    }
    free(stack);
    /* sort by path for the manifest */
    if (count > 1)
        qsort(list, count, sizeof(gf_manifest_entry), scan_cmp);
    *out = list;
    *n = count;
    return 0;
fail:
    free(stack);
    gf_manifest_free(list, count);
    (void)np;
    (void)all;
    return -1;
}

/* ------------------------------------------------------------- creation */

char *gf_package_create(const char *staging_dir, const gf_pkgmeta *meta,
                         const char *out_path, uint64_t epoch,
                         gf_manifest_entry **manifest_out, size_t *nentries_out)
{
    gf_manifest_entry *entries = NULL;
    size_t n = 0;
    if (scan_staging(staging_dir, &entries, &n) != 0) {
        gf_log(GF_LOG_ERROR, "package: staging scan failed");
        return NULL;
    }

    /* build a package root: METADATA, MANIFEST, CHECKSUMS + payload/ */
    char *tmproot = NULL;
    int tfd = gf_mkstemp_in("/tmp", &tmproot);
    if (tfd < 0) {
        gf_manifest_free(entries, n);
        return NULL;
    }
    close(tfd);
    unlink(tmproot);
    if (gf_fs_mkdir_p(tmproot) != 0) {
        free(tmproot);
        gf_manifest_free(entries, n);
        return NULL;
    }

    /* METADATA */
    gf_pkgmeta *m = gf_pkgmeta_copy(meta);
    gf_json *mjson = gf_pkgmeta_to_json(m);
    gf_pkgmeta_free_heap(m);
    char *bi = gf_json_dump_pretty(mjson);
    gf_json_free(mjson);
    if (!bi)
        goto fail_root;
    char *metadata_path = gf_path_join(tmproot, "METADATA");
    if (gf_fs_write_file_atomic(metadata_path, bi, strlen(bi)) != 0) {
        free(bi);
        free(metadata_path);
        goto fail_root;
    }
    free(metadata_path);
    free(bi);

    /* MANIFEST */
    {
        gf_json *arr = gf_json_new_array();
        for (size_t i = 0; i < n; i++) {
            gf_json *e = gf_json_new_object();
            gf_json_object_set(e, "path", gf_json_new_string(entries[i].path));
            char t[2] = { entries[i].type, 0 };
            gf_json_object_set(e, "type", gf_json_new_string(t));
            gf_json_object_set(e, "mode", gf_json_new_int((int64_t)entries[i].mode));
            gf_json_object_set(e, "size", gf_json_new_int((int64_t)entries[i].size));
            if (entries[i].sha256)
                gf_json_object_set(e, "sha256", gf_json_new_string(entries[i].sha256));
            if (entries[i].target)
                gf_json_object_set(e, "target", gf_json_new_string(entries[i].target));
            gf_json_array_push(arr, e);
        }
        gf_json *mobj = gf_json_new_object();
        gf_json_object_set(mobj, "entries", arr);
        char *dump = gf_json_dump_pretty(mobj);
        gf_json_free(mobj);
        char *mp = gf_path_join(tmproot, "MANIFEST");
        gf_fs_write_file_atomic(mp, dump, strlen(dump));
        free(mp);
        free(dump);
    }

    /* CHECKSUMS */
    {
        gf_strbuf sb;
        gf_strbuf_init(&sb);
        for (size_t i = 0; i < n; i++) {
            if (entries[i].type == 'f' && entries[i].sha256)
                gf_strbuf_appendf(&sb, "%s  %s\n", entries[i].sha256,
                                  entries[i].path);
        }
        char *cp = gf_path_join(tmproot, "CHECKSUMS");
        gf_fs_write_file_atomic(cp, sb.s, sb.len);
        gf_strbuf_free(&sb);
        free(cp);
    }

    /* payload/: hardlink the staging tree in (cheap + deterministic) */
    {
        char *pay = gf_path_join(tmproot, "payload");
        if (gf_fs_hardlink_tree(staging_dir, pay) != 0) {
            gf_log(GF_LOG_ERROR, "package: hardlink staging failed");
            free(pay);
            goto fail_root;
        }
        free(pay);
    }

    /* deterministic tar of tmproot */
    if (gf_tar_write_dir(tmproot, out_path, NULL, epoch) != 0) {
        gf_log(GF_LOG_ERROR, "package: tar write failed");
        goto fail_root;
    }
    char *build_id = gf_sha256_file_hex(out_path);
    gf_fs_rm_rf(tmproot);
    free(tmproot);
    if (manifest_out) {
        *manifest_out = entries;
        *nentries_out = n;
    } else {
        gf_manifest_free(entries, n);
    }
    return build_id;

fail_root:
    gf_fs_rm_rf(tmproot);
    free(tmproot);
    gf_manifest_free(entries, n);
    return NULL;
}

/* --------------------------------------------------------------- parsing */

int gf_package_open(const char *pkg_path, gf_pkgmeta **meta_out,
                    gf_manifest_entry **entries_out, size_t *nentries_out,
                    char **build_id_out)
{
    if (meta_out)
        *meta_out = NULL;
    if (entries_out)
        *entries_out = NULL;
    if (nentries_out)
        *nentries_out = 0;
    if (build_id_out)
        *build_id_out = NULL;

    gf_tar_entry *tents = NULL;
    size_t n = 0;
    gf_tar_limits lim = gf_tar_limits_default();
    if (gf_tar_list(pkg_path, &tents, &n, &lim) != 0) {
        gf_log(GF_LOG_ERROR, "package: archive validation failed: %s", pkg_path);
        return -1;
    }

    /* METADATA/MANIFEST must exist as entries */
    bool has_meta = false, has_manifest = false;
    for (size_t i = 0; i < n; i++) {
        if (tents[i].type == 'f' && strcmp(tents[i].path, "METADATA") == 0)
            has_meta = true;
        if (tents[i].type == 'f' && strcmp(tents[i].path, "MANIFEST") == 0)
            has_manifest = true;
    }
    if (!has_meta || !has_manifest) {
        gf_log(GF_LOG_ERROR, "package: missing METADATA/MANIFEST entry");
        gf_tar_entries_free(tents, n);
        return -1;
    }
    char *tmpdir = NULL;
    {
        int fd = gf_mkstemp_in("/tmp", &tmpdir);
        if (fd < 0) {
            gf_tar_entries_free(tents, n);
            return -1;
        }
        close(fd);
        unlink(tmpdir);
        if (gf_fs_mkdir_p(tmpdir) != 0) {
            free(tmpdir);
            gf_tar_entries_free(tents, n);
            return -1;
        }
        if (gf_tar_extract(pkg_path, tmpdir, 0, &lim, NULL) != 0) {
            gf_fs_rm_rf(tmpdir);
            free(tmpdir);
            gf_tar_entries_free(tents, n);
            return -1;
        }
    }

    char *meta_text = NULL, *man_text = NULL;
    char *mp = gf_path_join(tmpdir, "METADATA");
    size_t mlen = 0;
    meta_text = gf_fs_read_file(mp, &mlen);
    free(mp);
    if (!meta_text) {
        gf_log(GF_LOG_ERROR, "package: missing METADATA");
        goto fail;
    }
    gf_json *mjson = gf_json_parse(meta_text, mlen, NULL, 0);
    free(meta_text);
    if (!mjson) {
        gf_log(GF_LOG_ERROR, "package: invalid METADATA JSON");
        goto fail;
    }
    gf_pkgmeta *meta = gf_pkgmeta_from_json(mjson);
    gf_json_free(mjson);
    if (!meta) {
        gf_log(GF_LOG_ERROR, "package: incomplete METADATA");
        goto fail;
    }

    char *mnp = gf_path_join(tmpdir, "MANIFEST");
    size_t nlen = 0;
    man_text = gf_fs_read_file(mnp, &nlen);
    free(mnp);
    gf_manifest_entry *entries = NULL;
    size_t ne = 0;
    if (man_text) {
        gf_json *mj = gf_json_parse(man_text, nlen, NULL, 0);
        free(man_text);
        if (mj) {
            const gf_json *arr = gf_json_get(mj, "entries");
            if (arr && arr->type == GF_JSON_ARRAY) {
                ne = gf_json_len(arr);
                entries = gf_calloc(ne ? ne : 1, sizeof(gf_manifest_entry));
                for (size_t i = 0; i < ne; i++) {
                    const gf_json *e = gf_json_at(arr, i);
                    if (!e)
                        continue;
                    const char *p = gf_json_str(gf_json_get(e, "path"));
                    const char *t = gf_json_str(gf_json_get(e, "type"));
                    if (!p || !t || !*t) {
                        gf_log(GF_LOG_ERROR, "package: bad manifest entry");
                        gf_manifest_free(entries, ne);
                        gf_json_free(mj);
                        gf_pkgmeta_free_heap(meta);
                        goto fail;
                    }
                    entries[i].path = gf_strdup(p);
                    entries[i].type = t[0];
                    entries[i].mode = (uint32_t)gf_json_int(gf_json_get(e, "mode"), 0644);
                    entries[i].size = (uint64_t)gf_json_int(gf_json_get(e, "size"), 0);
                    const char *h = gf_json_str(gf_json_get(e, "sha256"));
                    entries[i].sha256 = h ? gf_strdup(h) : NULL;
                    const char *tg = gf_json_str(gf_json_get(e, "target"));
                    entries[i].target = tg ? gf_strdup(tg) : NULL;
                }
            }
            gf_json_free(mj);
        }
    }
    gf_fs_rm_rf(tmpdir);
    free(tmpdir);
    gf_tar_entries_free(tents, n);

    if (meta_out)
        *meta_out = meta;
    else
        gf_pkgmeta_free_heap(meta);
    if (entries_out)
        *entries_out = entries;
    else
        gf_manifest_free(entries, ne);
    if (nentries_out)
        *nentries_out = ne;
    if (build_id_out)
        *build_id_out = gf_sha256_file_hex(pkg_path);
    return 0;

fail:
    gf_fs_rm_rf(tmpdir);
    free(tmpdir);
    gf_tar_entries_free(tents, n);
    return -1;
}

int gf_package_extract_payload(const char *pkg_path, const char *dest_dir)
{
    gf_tar_limits lim = gf_tar_limits_default();
    /* extract whole package then move payload contents into dest */
    char *tmp = NULL;
    int fd = gf_mkstemp_in("/tmp", &tmp);
    if (fd < 0)
        return -1;
    close(fd);
    unlink(tmp);
    if (gf_fs_mkdir_p(tmp) != 0) {
        free(tmp);
        return -1;
    }
    if (gf_tar_extract(pkg_path, tmp, 0, &lim, NULL) != 0) {
        gf_fs_rm_rf(tmp);
        free(tmp);
        return -1;
    }
    char *pay = gf_path_join(tmp, "payload");
    if (!gf_fs_is_dir(pay)) {
        gf_log(GF_LOG_ERROR, "package: no payload directory");
        gf_fs_rm_rf(tmp);
        free(tmp);
        free(pay);
        return -1;
    }
    if (gf_fs_mkdir_p(dest_dir) != 0) {
        gf_fs_rm_rf(tmp);
        free(tmp);
        free(pay);
        return -1;
    }
    size_t n = 0;
    char **entries = NULL;
    if (gf_fs_list_dir(pay, &entries, &n) != 0) {
        gf_fs_rm_rf(tmp);
        free(tmp);
        free(pay);
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        char *b = gf_path_basename(entries[i]);
        char *dst = gf_path_join(dest_dir, b);
        struct stat st;
        if (lstat(entries[i], &st) == 0 && S_ISDIR(st.st_mode))
            rc = gf_fs_hardlink_tree(entries[i], dst);
        else
            rc = gf_fs_hardlink(entries[i], dst) == 0 ? 0 : -1;
        if (rc != 0)
            gf_log_errno(GF_LOG_ERROR, "payload extract");
        free(dst);
        free(b);
        free(entries[i]);
    }
    free(entries);
    gf_fs_rm_rf(tmp);
    free(tmp);
    free(pay);
    return rc;
}

int gf_package_verify(const char *pkg_path, const char *verify_dir)
{
    gf_pkgmeta *meta = NULL;
    gf_manifest_entry *entries = NULL;
    size_t ne = 0;
    char *bid = NULL;
    if (gf_package_open(pkg_path, &meta, &entries, &ne, &bid) != 0)
        return -1;

    /* build id must match METADATA's package_sha256 when present */
    if (meta->pkg_sha && bid && strcmp(meta->pkg_sha, bid) != 0) {
        gf_log(GF_LOG_ERROR, "package: build id mismatch (expected %s, is %s)",
               meta->pkg_sha, bid);
        goto fail;
    }
    int rc = 0;
    for (size_t i = 0; i < ne && rc == 0; i++) {
        if (entries[i].type != 'f')
            continue;
        char *fp = verify_dir
            ? gf_path_join(verify_dir, entries[i].path)
            : NULL;
        if (!fp) {
            /* no extracted dir given: we cannot verify files here */
            continue;
        }
        char *h = gf_sha256_file_hex(fp);
        if (!h || strcmp(h, entries[i].sha256 ? entries[i].sha256 : "") != 0) {
            gf_log(GF_LOG_ERROR, "package: checksum mismatch for %s",
                   entries[i].path);
            rc = -1;
        }
        free(h);
        free(fp);
    }
    gf_pkgmeta_free_heap(meta);
    gf_manifest_free(entries, ne);
    free(bid);
    return rc;
fail:
    gf_pkgmeta_free_heap(meta);
    gf_manifest_free(entries, ne);
    free(bid);
    return -1;
}
