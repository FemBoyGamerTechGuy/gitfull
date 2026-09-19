/* forge.c — forge dispatch, shared JSON access with backoff, release helpers. */
#include "forge.h"

#include "common.h"
#include "http.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

gf_forge *gf_forge_open(const gf_repo *repo, const gf_config *cfg)
{
    (void)cfg;
    if (!repo)
        return NULL;
    const struct gf_forge_ops *ops = NULL;
    if (gf_str_ieq(repo->kind, "github"))
        ops = &gf_forge_ops_github;
    else if (gf_str_ieq(repo->kind, "gitlab"))
        ops = &gf_forge_ops_gitlab;
    else if (gf_str_ieq(repo->kind, "gitea"))
        ops = &gf_forge_ops_gitea;
    else if (gf_str_ieq(repo->kind, "generic"))
        ops = &gf_forge_ops_generic;
    if (!ops) {
        gf_log(GF_LOG_ERROR, "unknown forge kind: %s", repo->kind);
        return NULL;
    }
    gf_forge *f = gf_calloc(1, sizeof(gf_forge));
    f->repo = repo;
    f->ops = ops;
    f->token = gf_repo_token(repo);
    f->http = gf_http_opts_default();
    f->http.max_bytes = 64u * 1024u * 1024u;
    return f;
}

void gf_forge_close(gf_forge *f)
{
    if (!f)
        return;
    free(f->token);
    free(f);
}

int gf_forge_api_get_json(gf_forge *f, const char *url, gf_json **out)
{
    *out = NULL;
    gf_http_opts o = f->http;
    o.token = f->token;
    char *token_host = NULL;
    if (f->token && f->repo && f->repo->api_url) {
        char *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
        if (gf_url_split(f->repo->api_url, &scheme, &host, &port, &path) == 0 &&
            host)
            token_host = gf_strdup(host);
        free(scheme);
        free(host);
        free(port);
        free(path);
    }
    o.token_host = token_host;

    gf_json *j = NULL;
    int code = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        char *body = NULL;
        size_t len = 0;
        code = gf_http_get(url, &o, &body, &len);
        if (code == 200) {
            char err[128];
            j = gf_json_parse(body, len, err, sizeof(err));
            free(body);
            if (!j) {
                gf_log(GF_LOG_ERROR, "forge: invalid JSON from %s: %s", url, err);
                free(token_host);
                return -1;
            }
            *out = j;
            free(token_host);
            return 0;
        }
        free(body);
        /* Retry transient failures. 429/5xx are classic; 403 is retried
         * because some egress proxies emit empty-body 403 hiccups (real
         * GitHub rate limits carry a JSON body and a retry will not help,
         * but we cap attempts anyway). */
        if (code == 429 || code >= 500 || code == 403) {
            int delay = attempt == 0 ? 1 : (attempt == 1 ? 2 : (attempt == 2 ? 5 : 15));
            if (attempt < 3) {
                gf_log(GF_LOG_WARN, "forge: %s returned %d, retrying in %ds",
                       f->repo->name, code, delay);
                struct timespec ts = { delay, 0 };
                nanosleep(&ts, NULL);
                continue;
            }
        }
        break; /* non-retryable or attempts exhausted */
    }
    if (code == 404) {
        gf_log(GF_LOG_DEBUG, "forge: 404 for %s", url);
        free(token_host);
        return 1; /* not found */
    }
    gf_log(GF_LOG_ERROR, "forge: request failed (%d): %s", code, url);
    free(token_host);
    return -1;
}

/* -------------------------------------------------------- release helpers */

void gf_forge_rels_free(gf_rel *rels, size_t n)
{
    if (!rels)
        return;
    for (size_t i = 0; i < n; i++) {
        free(rels[i].tag);
        free(rels[i].published_at);
        free(rels[i].commit);
        gf_json_free(rels[i].assets);
    }
    free(rels);
}

/* Extract a normalized gf_rel list from a provider's JSON releases array.
 * Fields read per item: tag/tag_name/name, prerelease, draft, published_at,
 * commit/sha1 (gitlab), assets (github/gitea). Unknown shapes are skipped. */
int gf_forge_rels_from_json(const gf_json *arr, gf_rel **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    if (!arr || arr->type != GF_JSON_ARRAY)
        return -1;
    size_t len = gf_json_len(arr);
    gf_rel *rels = gf_calloc(len ? len : 1, sizeof(gf_rel));
    for (size_t i = 0; i < len; i++) {
        const gf_json *item = gf_json_at(arr, i);
        if (!item || item->type != GF_JSON_OBJECT)
            continue;
        const char *tag = gf_json_str(gf_json_get(item, "tag_name"));
        if (!tag)
            tag = gf_json_str(gf_json_get(item, "tag"));
        if (!tag || !*tag)
            continue;
        gf_rel *r = &rels[*n];
        r->tag = gf_strdup(tag);
        r->prerelease = gf_json_bool(gf_json_get(item, "prerelease"), false);
        r->draft = gf_json_bool(gf_json_get(item, "draft"), false);
        /* GitLab calls drafts "upcoming_release" */
        if (gf_json_bool(gf_json_get(item, "upcoming_release"), false))
            r->prerelease = true;
        const char *pub = gf_json_str(gf_json_get(item, "published_at"));
        if (!pub)
            pub = gf_json_str(gf_json_get(item, "created_at"));
        r->published_at = pub ? gf_strdup(pub) : NULL;
        const char *commit = gf_json_str(gf_json_get(item, "commit"));
        if (!commit) {
            const gf_json *cj = gf_json_get(item, "commit");
            if (cj && cj->type == GF_JSON_OBJECT)
                commit = gf_json_str(gf_json_get(cj, "sha"));
            if (!commit) {
                const gf_json *target = gf_json_get(item, "target");
                if (target && target->type == GF_JSON_OBJECT)
                    commit = gf_json_str(gf_json_get(target, "sha"));
            }
        }
        if (!commit) {
            /* GitHub tags list: {"commit": {"sha": ...}} */
            const gf_json *cj = gf_json_get(item, "commit");
            if (cj && cj->type == GF_JSON_OBJECT)
                commit = gf_json_str(gf_json_get(cj, "sha"));
        }
        r->commit = commit ? gf_strdup(commit) : NULL;
        const gf_json *assets = gf_json_get(item, "assets");
        if (assets && assets->type == GF_JSON_ARRAY) {
            char *dump = gf_json_dump(assets);
            r->assets = gf_json_parse(dump, strlen(dump), NULL, 0);
            free(dump);
        } else {
            r->assets = gf_json_new_array();
        }
        (*n)++;
    }
    *out = rels;
    return 0;
}

int gf_forge_select_release(const gf_rel *rels, size_t n, bool allow_prerelease,
                            const gf_rel **out)
{
    *out = NULL;
    const gf_rel *best = NULL;
    gf_version best_v;
    bool have = false;
    for (size_t i = 0; i < n; i++) {
        const gf_rel *r = &rels[i];
        if (r->draft)
            continue;
        if (r->prerelease && !allow_prerelease)
            continue;
        gf_version v;
        if (!gf_version_parse(r->tag, &v))
            continue;
        if (!have || gf_version_cmp(&v, &best_v) > 0) {
            if (have)
                gf_version_free(&best_v);
            best_v = v;
            have = true;
            best = r;
        } else {
            gf_version_free(&v);
        }
    }
    if (have)
        gf_version_free(&best_v);
    if (!best)
        return -1;
    *out = best;
    return 0;
}

const gf_rel *gf_forge_find_release(const gf_rel *rels, size_t n,
                                    const char *tag)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(rels[i].tag, tag) == 0)
            return &rels[i];
    }
    return NULL;
}
