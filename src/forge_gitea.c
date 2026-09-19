/* forge_gitea.c — Gitea/Codeberg API v1 provider. */
#include "forge.h"

#include "common.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *api_url(gf_forge *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static char *api_url(gf_forge *f, const char *fmt, ...)
{
    char path[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    char *base = gf_strdup(f->repo->api_url);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        base[--bl] = '\0';
    char *url = gf_path_join(base, path);
    free(base);
    return url;
}

static int gt_repo_info(gf_forge *f, const char *owner, const char *repo,
                        char **default_branch, char **description,
                        char **license)
{
    *default_branch = *description = *license = NULL;
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *url = api_url(f, "/repos/%s/%s", o, r);
    free(o);
    free(r);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        return rc;
    }
    const char *b = gf_json_str(gf_json_get(j, "default_branch"));
    if (b)
        *default_branch = gf_strdup(b);
    const char *d = gf_json_str(gf_json_get(j, "description"));
    if (d)
        *description = gf_strdup(d);
    /* Gitea has no structured license field */
    gf_json_free(j);
    return 0;
}

static int gt_list_releases(gf_forge *f, const char *owner, const char *repo,
                            gf_json **out)
{
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    gf_json *all = gf_json_new_array();
    for (int page = 1; page <= 20; page++) {
        char *url = api_url(f, "/repos/%s/%s/releases?limit=50&page=%d", o, r,
                            page);
        gf_json *j = NULL;
        int rc = gf_forge_api_get_json(f, url, &j);
        free(url);
        if (rc != 0) {
            gf_json_free(j);
            gf_json_free(all);
            free(o);
            free(r);
            return rc;
        }
        if (j->type != GF_JSON_ARRAY) {
            gf_json_free(j);
            break;
        }
        size_t n = gf_json_len(j);
        for (size_t i = 0; i < n; i++) {
            char *dump = gf_json_dump(gf_json_at(j, i));
            gf_json *item = gf_json_parse(dump, strlen(dump), NULL, 0);
            free(dump);
            if (item)
                gf_json_array_push(all, item);
        }
        gf_json_free(j);
        if (n == 0)
            break;
    }
    free(o);
    free(r);
    *out = all;
    return 0;
}

static int gt_list_tags(gf_forge *f, const char *owner, const char *repo,
                        gf_json **out)
{
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    gf_json *all = gf_json_new_array();
    for (int page = 1; page <= 20; page++) {
        char *url = api_url(f, "/repos/%s/%s/tags?limit=50&page=%d", o, r, page);
        gf_json *j = NULL;
        int rc = gf_forge_api_get_json(f, url, &j);
        free(url);
        if (rc != 0) {
            gf_json_free(j);
            gf_json_free(all);
            free(o);
            free(r);
            return rc;
        }
        if (j->type != GF_JSON_ARRAY) {
            gf_json_free(j);
            break;
        }
        size_t n = gf_json_len(j);
        for (size_t i = 0; i < n; i++) {
            char *dump = gf_json_dump(gf_json_at(j, i));
            gf_json *item = gf_json_parse(dump, strlen(dump), NULL, 0);
            free(dump);
            if (item)
                gf_json_array_push(all, item);
        }
        gf_json_free(j);
        if (n == 0)
            break;
    }
    free(o);
    free(r);
    *out = all;
    return 0;
}

static int gt_branch_head(gf_forge *f, const char *owner, const char *repo,
                          const char *branch, char *sha_out)
{
    sha_out[0] = '\0';
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *b = gf_url_encode(branch);
    char *url = api_url(f, "/repos/%s/%s/branches/%s", o, r, b);
    free(o);
    free(r);
    free(b);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        return rc;
    }
    const gf_json *commit = gf_json_get(j, "commit");
    const char *sha = NULL;
    if (commit)
        sha = gf_json_str(gf_json_get(commit, "id"));
    if (!sha || strlen(sha) < 40) {
        gf_json_free(j);
        return -1;
    }
    memcpy(sha_out, sha, 40);
    sha_out[40] = '\0';
    gf_json_free(j);
    return 0;
}

static char *gt_tarball_url(gf_forge *f, const char *owner, const char *repo,
                            const char *tag)
{
    /* {web}/{owner}/{repo}/archive/{tag}.tar.gz */
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *t = gf_url_encode(tag);
    char *base = gf_strdup(f->repo->web_url ? f->repo->web_url
                                            : f->repo->api_url);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        base[--bl] = '\0';
    char *url = NULL;
    if (asprintf(&url, "%s/%s/%s/archive/%s.tar.gz", base, o, r, t) < 0)
        url = NULL;
    free(base);
    free(o);
    free(r);
    free(t);
    return url;
}

static int gt_release_assets(gf_forge *f, const char *owner, const char *repo,
                             const char *tag, gf_json **out)
{
    /* Gitea release assets come embedded in the release object; the generic
     * release listing already carries them. Return empty here; source
     * verification uses the tarball URL + commit identity. */
    (void)f;
    (void)owner;
    (void)repo;
    (void)tag;
    *out = gf_json_new_array();
    return 0;
}

static int gt_search(gf_forge *f, const char *query, gf_json **out)
{
    char *q = gf_url_encode(query);
    char *url = api_url(f, "/repos/search?q=%s&limit=30", q);
    free(q);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        return rc;
    }
    /* Gitea wraps in {"data": [...]} */
    const gf_json *data = gf_json_get(j, "data");
    if (data && data->type == GF_JSON_ARRAY) {
        *out = gf_json_parse(gf_json_dump(data), strlen(gf_json_dump(data)),
                             NULL, 0);
        gf_json_free(j);
        return 0;
    }
    *out = j ? j : gf_json_new_array();
    return 0;
}

const struct gf_forge_ops gf_forge_ops_gitea = {
    .kind = "gitea",
    .repo_info = gt_repo_info,
    .list_releases = gt_list_releases,
    .list_tags = gt_list_tags,
    .branch_head = gt_branch_head,
    .tarball_url = gt_tarball_url,
    .release_assets = gt_release_assets,
    .search = gt_search,
};
