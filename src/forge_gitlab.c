/* forge_gitlab.c — GitLab REST API v4 provider (gitlab.com and self-hosted). */
#include "forge.h"

#include "common.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *gl_project_path(gf_forge *f, const char *owner, const char *repo)
{
    char *o = gf_url_encode(owner);
    char *r = gf_url_encode(repo);
    (void)f;
    char *id = NULL;
    if (asprintf(&id, "%s%%2F%s", o, r) < 0)
        id = NULL;
    free(o);
    free(r);
    return id;
}

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

static int gl_repo_info(gf_forge *f, const char *owner, const char *repo,
                        char **default_branch, char **description,
                        char **license)
{
    *default_branch = *description = *license = NULL;
    char *id = gl_project_path(f, owner, repo);
    if (!id)
        return -1;
    char *url = api_url(f, "/projects/%s", id);
    free(id);
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
        const char *name = gf_json_str(gf_json_get(lic, "name"));
        if (name)
            *license = gf_strdup(name);
    }
    gf_json_free(j);
    return 0;
}

static int gl_list_releases(gf_forge *f, const char *owner, const char *repo,
                            gf_json **out)
{
    char *id = gl_project_path(f, owner, repo);
    if (!id)
        return -1;
    gf_json *all = gf_json_new_array();
    for (int page = 1; page <= 20; page++) {
        char *url = api_url(f, "/projects/%s/releases?per_page=100&page=%d",
                            id, page);
        gf_json *j = NULL;
        int rc = gf_forge_api_get_json(f, url, &j);
        free(url);
        if (rc != 0) {
            gf_json_free(j);
            gf_json_free(all);
            free(id);
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
    free(id);
    *out = all;
    return 0;
}

static int gl_list_tags(gf_forge *f, const char *owner, const char *repo,
                        gf_json **out)
{
    char *id = gl_project_path(f, owner, repo);
    if (!id)
        return -1;
    gf_json *all = gf_json_new_array();
    for (int page = 1; page <= 20; page++) {
        char *url = api_url(f,
                            "/projects/%s/repository/tags?per_page=100&page=%d",
                            id, page);
        gf_json *j = NULL;
        int rc = gf_forge_api_get_json(f, url, &j);
        free(url);
        if (rc != 0) {
            gf_json_free(j);
            gf_json_free(all);
            free(id);
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
    free(id);
    *out = all;
    return 0;
}

static int gl_branch_head(gf_forge *f, const char *owner, const char *repo,
                          const char *branch, char *sha_out)
{
    sha_out[0] = '\0';
    char *id = gl_project_path(f, owner, repo);
    if (!id)
        return -1;
    char *b = gf_url_encode(branch);
    char *url = api_url(f, "/projects/%s/repository/branches/%s", id, b);
    free(b);
    free(id);
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

static char *gl_tarball_url(gf_forge *f, const char *owner, const char *repo,
                            const char *tag)
{
    /* {web}/-/archive/{tag}/{repo}-{tag}.tar.gz (API also offers
     * /projects/:id/repository/archive.tar.gz?sha=) */
    char *id = gl_project_path(f, owner, repo);
    if (!id)
        return NULL;
    char *url = api_url(f, "/projects/%s/repository/archive.tar.gz?sha=%s",
                        id, tag);
    free(id);
    return url;
}

static int gl_release_assets(gf_forge *f, const char *owner, const char *repo,
                             const char *tag, gf_json **out)
{
    char *id = gl_project_path(f, owner, repo);
    if (!id)
        return -1;
    char *t = gf_url_encode(tag);
    char *url = api_url(f, "/projects/%s/releases/%s/assets/links", id, t);
    free(t);
    free(id);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        /* GitLab releases may not exist; empty list is fine */
        *out = gf_json_new_array();
        return 0;
    }
    *out = j ? j : gf_json_new_array();
    return 0;
}

static int gl_search(gf_forge *f, const char *query, gf_json **out)
{
    char *q = gf_url_encode(query);
    char *url = api_url(f, "/search?scope=projects&search=%s&per_page=30", q);
    free(q);
    gf_json *j = NULL;
    int rc = gf_forge_api_get_json(f, url, &j);
    free(url);
    if (rc != 0) {
        gf_json_free(j);
        return rc;
    }
    *out = j ? j : gf_json_new_array();
    return 0;
}

const struct gf_forge_ops gf_forge_ops_gitlab = {
    .kind = "gitlab",
    .repo_info = gl_repo_info,
    .list_releases = gl_list_releases,
    .list_tags = gl_list_tags,
    .branch_head = gl_branch_head,
    .tarball_url = gl_tarball_url,
    .release_assets = gl_release_assets,
    .search = gl_search,
};
