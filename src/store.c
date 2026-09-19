/* store.c — content-addressed package store operations. */
#include "store.h"

#include "common.h"
#include "sha256.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool safe_name(const char *s)
{
    if (!gf_path_component_ok(s))
        return false;
    /* also reject dotfiles and plain dots inside names */
    if (s[0] == '.')
        return false;
    return true;
}

char *gf_store_pkg_path(const char *state_dir, const char *name,
                        const char *version, const char *build_id)
{
    if (!safe_name(name) || !safe_name(version) || !safe_name(build_id)) {
        gf_log(GF_LOG_ERROR, "store: unsafe package coordinates");
        return NULL;
    }
    return gf_path_join_multi(state_dir, "store", name, version, build_id,
                              "pkg.gfpkg", NULL);
}

char *gf_store_files_path(const char *state_dir, const char *name,
                          const char *version, const char *build_id)
{
    if (!safe_name(name) || !safe_name(version) || !safe_name(build_id)) {
        gf_log(GF_LOG_ERROR, "store: unsafe package coordinates");
        return NULL;
    }
    return gf_path_join_multi(state_dir, "store", name, version, build_id,
                              "files", NULL);
}

bool gf_store_has(const char *state_dir, const char *name,
                  const char *version, const char *build_id)
{
    char *p = gf_store_pkg_path(state_dir, name, version, build_id);
    bool has = p && gf_fs_is_file(p);
    free(p);
    return has;
}

int gf_store_put(const char *state_dir, const char *pkg_file,
                 const gf_pkgmeta *meta)
{
    char *dir = gf_path_join_multi(state_dir, "store", meta->name,
                                   meta->version, meta->build_id, NULL);
    if (!dir) {
        gf_log(GF_LOG_ERROR, "store: unsafe package coordinates");
        return -1;
    }
    char *pkg_dst = gf_path_join(dir, "pkg.gfpkg");
    char *files_dst = gf_path_join(dir, "files");

    if (gf_store_has(state_dir, meta->name, meta->version, meta->build_id)) {
        /* already present: verify identity still matches */
        char *have = gf_sha256_file_hex(pkg_dst);
        bool same = have && meta->pkg_sha && strcmp(have, meta->pkg_sha) == 0;
        free(have);
        if (same) {
            free(dir);
            free(pkg_dst);
            free(files_dst);
            return 0;
        }
        gf_log(GF_LOG_ERROR,
               "store: build-id collision for %s %s (content differs)",
               meta->name, meta->version);
        free(dir);
        free(pkg_dst);
        free(files_dst);
        return -1;
    }

    if (gf_fs_mkdir_p(dir) != 0) {
        free(dir);
        free(pkg_dst);
        free(files_dst);
        return -1;
    }

    /* move the package file into the store (atomic rename within state) */
    if (rename(pkg_file, pkg_dst) != 0) {
        gf_log_errno(GF_LOG_ERROR, "store: rename");
        /* cross-device fallback: copy */
        if (gf_fs_copy_file(pkg_file, pkg_dst, 0644) != 0) {
            free(dir);
            free(pkg_dst);
            free(files_dst);
            return -1;
        }
        unlink(pkg_file);
    }

    /* extract payload for activation hardlinks */
    if (gf_fs_mkdir_p(files_dst) != 0 ||
        gf_package_extract_payload(pkg_dst, files_dst) != 0) {
        gf_log(GF_LOG_ERROR, "store: payload extraction failed");
        gf_fs_rm_rf(dir);
        free(dir);
        free(pkg_dst);
        free(files_dst);
        return -1;
    }
    free(dir);
    free(pkg_dst);
    free(files_dst);
    return 0;
}

char **gf_store_versions(const char *state_dir, const char *name, size_t *n)
{
    *n = 0;
    if (!safe_name(name))
        return NULL;
    char *dir = gf_path_join_multi(state_dir, "store", name, NULL);
    size_t count = 0;
    char **out = NULL;
    if (gf_fs_list_dir(dir, &out, &count) != 0) {
        free(dir);
        return NULL;
    }
    free(dir);
    /* keep only version dirs, sort newest first */
    size_t w = 0;
    for (size_t i = 0; i < count; i++) {
        if (gf_fs_is_dir(out[i])) {
            char *b = gf_path_basename(out[i]);
            free(out[i]);
            out[w] = b ? b : out[i];
            if (b)
                w++;
        } else {
            free(out[i]);
        }
    }
    /* simple insertion sort by version */
    for (size_t i = 1; i < w; i++) {
        char *key = out[i];
        gf_version kv;
        bool kp = gf_version_parse(key, &kv);
        size_t j = i;
        while (j > 0) {
            gf_version pv;
            bool pp = gf_version_parse(out[j - 1], &pv);
            bool swap = false;
            if (kp && pp)
                swap = gf_version_cmp(&kv, &pv) > 0;
            else if (kp != pp)
                swap = kp; /* versions before junk */
            else
                swap = strcmp(key, out[j - 1]) > 0;
            if (!swap)
                break;
            out[j] = out[j - 1];
            j--;
            gf_version_free(&pv);
        }
        out[j] = key;
        if (kp)
            gf_version_free(&kv);
    }
    *n = w;
    if (w == 0) {
        free(out);
        return NULL;
    }
    return out;
}

int gf_store_remove_version(const char *state_dir, const char *name,
                            const char *version, const char *build_id)
{
    if (!safe_name(name) || !safe_name(version) || !safe_name(build_id))
        return -1;
    char *dir = gf_path_join_multi(state_dir, "store", name, version, NULL);
    if (gf_fs_rm_rf(dir) != 0) {
        free(dir);
        return -1;
    }
    free(dir);
    /* remove empty parents */
    char *pdir = gf_path_join_multi(state_dir, "store", name, NULL);
    gf_fs_rmdir_if_empty(pdir);
    free(pdir);
    return 0;
}

uint64_t gf_store_usage(const char *state_dir)
{
    char *dir = gf_path_join(state_dir, "store");
    uint64_t bytes = 0;
    gf_fs_du(dir, &bytes);
    free(dir);
    return bytes;
}
