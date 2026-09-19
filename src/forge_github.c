/* forge_github.c — GitHub REST API v3 provider. */
#include "forge.h"

#include "common.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *api_join(const gf_forge *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static char *api_join(const gf_forge *f, const char *fmt, ...)
{
    char path[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    /* strip trailing slashes from api base */
    char *base = gf_strdup(f->repo->api_url);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        base[--bl] = '\0';
    char *url = gf_path_join(base, path);
    free(base);
    return url;
}

static int gh_repo_info(gf_forge *f, const char *owner, const char *repo,
                        char **default_branch, char **description,
                        char **license)
{
    *default_branch = *description = *license = NULL;
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *url = api_join(f, "/repos/%s/%s", o, r);
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
    const gf_json *lic = gf_json_get(j, "license");
    if (lic) {
        const char *spdx = gf_json_str(gf_json_get(lic, "spdx_id"));
        if (spdx)
            *license = gf_strdup(spdx);
    }
    gf_json_free(j);
    return 0;
}

static int gh_list_releases(gf_forge *f, const char *owner, const char *repo,
                            gf_json **out)
{
    /* /repos/{owner}/{repo}/releases — paginated */
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    gf_json *all = gf_json_new_array();
    for (int page = 1; page <= 20; page++) {
        char *url = api_join(f, "/repos/%s/%s/releases?per_page=100&page=%d",
                             o, r, page);
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

static int gh_list_tags(gf_forge *f, const char *owner, const char *repo,
                        gf_json **out)
{
    /* the tags path differs: /repos/{o}/{r}/tags */
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *base = gf_strdup(f->repo->api_url);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        base[--bl] = '\0';
    gf_json *all = gf_json_new_array();
    for (int page = 1; page <= 20; page++) {
        char url[1400];
        snprintf(url, sizeof(url), "%s/repos/%s/%s/tags?per_page=100&page=%d",
                 base, o, r, page);
        gf_json *j = NULL;
        int rc = gf_forge_api_get_json(f, url, &j);
        if (rc != 0) {
            gf_json_free(j);
            gf_json_free(all);
            free(base);
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
    free(base);
    free(o);
    free(r);
    *out = all;
    return 0;
}

static int gh_branch_head(gf_forge *f, const char *owner, const char *repo,
                          const char *branch, char *sha_out)
{
    sha_out[0] = '\0';
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *b = gf_url_encode(branch);
    char *url = api_join(f, "/repos/%s/%s/commits/%s", o, r, b);
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
    const char *sha = gf_json_str(gf_json_get(j, "sha"));
    if (!sha || strlen(sha) < 40) {
        gf_json_free(j);
        return -1;
    }
    memcpy(sha_out, sha, 40);
    sha_out[40] = '\0';
    gf_json_free(j);
    return 0;
}

static char *gh_tarball_url(gf_forge *f, const char *owner, const char *repo,
                            const char *tag)
{
    (void)f;
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *t = gf_url_encode(tag);
    char *url = NULL;
    if (asprintf(&url, "https://codeload.github.com/%s/%s/tar.gz/%s", o, r,
                 t) < 0)
        url = NULL;
    free(o);
    free(r);
    free(t);
    return url;
}

static int gh_release_assets(gf_forge *f, const char *owner, const char *repo,
                             const char *tag, gf_json **out)
{
    /* releases list filtered by tag: /releases/tags/{tag} */
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    char *t = gf_url_encode(tag);
    char *url = api_join(f, "/repos/%s/%s/releases/tags/%s", o, r, t);
    free(o);
    free(r);
    free(t);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        return rc;
    }
    const gf_json *assets = gf_json_get(j, "assets");
    gf_json *res = gf_json_new_array();
    if (assets && assets->type == GF_JSON_ARRAY) {
        size_t n = gf_json_len(assets);
        for (size_t i = 0; i < n; i++) {
            char *dump = gf_json_dump(gf_json_at(assets, i));
            gf_json *item = gf_json_parse(dump, strlen(dump), NULL, 0);
            free(dump);
            if (item)
                gf_json_array_push(res, item);
        }
    }
    gf_json_free(j);
    *out = res;
    return 0;
}

static int gh_search(gf_forge *f, const char *query, gf_json **out)
{
    char *q = gf_url_encode(query);
    char *url = api_join(f, "/search/repositories?q=%s&sort=stars&order=desc&per_page=30", q);
    free(q);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        return rc;
    }
    const gf_json *items = gf_json_get(j, "items");
    gf_json *res = gf_json_new_array();
    if (items && items->type == GF_JSON_ARRAY) {
        size_t n = gf_json_len(items);
        for (size_t i = 0; i < n; i++) {
            char *dump = gf_json_dump(gf_json_at(items, i));
            gf_json *item = gf_json_parse(dump, strlen(dump), NULL, 0);
            free(dump);
            if (item)
                gf_json_array_push(res, item);
        }
    }
    gf_json_free(j);
    *out = res;
    return 0;
}

const struct gf_forge_ops gf_forge_ops_github = {
    .kind = "github",
    .repo_info = gh_repo_info,
    .list_releases = gh_list_releases,
    .list_tags = gh_list_tags,
    .branch_head = gh_branch_head,
    .tarball_url = gh_tarball_url,
    .release_assets = gh_release_assets,
    .search = gh_search,
};
