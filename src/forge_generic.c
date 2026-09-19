/* forge_generic.c — provider for plain Git servers (no API).
 *
 * Everything is resolved via `git ls-remote`, which needs no API and works
 * with any Git server. Releases are unknown (tags only); a tag is treated
 * as a candidate release and stable-selection still applies semver rules.
 */
#include "forge.h"

#include "common.h"
#include "gitx.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *generic_clone_url(gf_forge *f, const char *owner,
                               const char *repo)
{
    char *base = gf_strdup(f->repo->web_url ? f->repo->web_url
                                            : f->repo->api_url);
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        base[--bl] = '\0';
    char *url;
    if (owner && *owner) {
        if (asprintf(&url, "%s/%s/%s.git", base, owner, repo) < 0)
            url = NULL;
    } else {
        if (asprintf(&url, "%s/%s.git", base, repo) < 0)
            url = NULL;
    }
    free(base);
    return url;
}

static int gen_repo_info(gf_forge *f, const char *owner, const char *repo,
                         char **default_branch, char **description,
                         char **license)
{
    *description = *license = NULL;
    *default_branch = NULL;
    char *url = generic_clone_url(f, owner, repo);
    if (!url)
        return -1;
    char sha[41];
    int rc = gf_git_ls_remote_head(url, default_branch, sha, f->token);
    free(url);
    if (rc != 0)
        return 1; /* not found / unreachable */
    return 0;
}

static int gen_list_tags(gf_forge *f, const char *owner, const char *repo,
                         gf_json **out)
{
    char *url = generic_clone_url(f, owner, repo);
    if (!url)
        return -1;
    const char *argv[] = { "git", "ls-remote", "--tags", "--", url, NULL };
    gf_strbuf out_s, err_s;
    gf_strbuf_init(&out_s);
    gf_strbuf_init(&err_s);
    /* plain env (token handled via askpass in git_run_token) */
    gf_exec_opts eo = gf_exec_opts_default();
    char *envvars[8] = { NULL };
    eo.envp = envvars[0] ? envvars : NULL;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    /* use gitx's token-safe runner via ls-remote per-tag would be slow; run
     * once with all refs: */
    const char *argv_all[] = { "git", "ls-remote", "--tags", "--", url, NULL };
    (void)argv;
    eo.out = &out_s;
    eo.err = &err_s;
    /* set clean env vars like gitx does */
    static char EV_PROMPT[] = "GIT_TERMINAL_PROMPT=0";
    static char EV_NOSYSTEM[] = "GIT_CONFIG_NOSYSTEM=1";
    static char EV_NOGLOBAL[] = "GIT_CONFIG_GLOBAL=/dev/null";
    static char EV_LC[] = "LC_ALL=C";
    static char EV_TZ[] = "TZ=UTC";
    char *env[8] = { EV_PROMPT, EV_NOSYSTEM, EV_NOGLOBAL, EV_LC, EV_TZ, NULL, NULL };
    eo.envp = env;
    if (gf_exec_capture(argv_all, &eo, &res) != 0 || res.status != 0) {
        gf_log(GF_LOG_ERROR, "ls-remote failed for %s: %s", url,
               gf_strbuf_str(&err_s));
        gf_strbuf_free(&out_s);
        gf_strbuf_free(&err_s);
        free(url);
        return -1;
    }
    gf_strbuf_free(&err_s);
    free(url);

    gf_json *arr = gf_json_new_array();
    char *text = gf_strbuf_steal(&out_s);
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        /* <sha>\trefs/tags/<name>[^{}] */
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        const char *ref = tab + 1;
        if (!gf_str_starts_with(ref, "refs/tags/"))
            continue;
        const char *name = ref + strlen("refs/tags/");
        if (gf_str_ends_with(name, "^{}"))
            continue; /* peeled entries: keep the tag object line */
        gf_json *obj = gf_json_new_object();
        gf_json_object_set(obj, "name", gf_json_new_string(name));
        gf_json *commit = gf_json_new_object();
        gf_json_object_set(commit, "sha", gf_json_new_string(line));
        gf_json_object_set(obj, "commit", commit);
        gf_json_array_push(arr, obj);
    }
    free(text);
    *out = arr;
    return 0;
}

static int gen_list_releases(gf_forge *f, const char *owner, const char *repo,
                             gf_json **out)
{
    /* no release API: releases == tags (stable selection still applies) */
    return gen_list_tags(f, owner, repo, out);
}

static int gen_branch_head(gf_forge *f, const char *owner, const char *repo,
                           const char *branch, char *sha_out)
{
    char *url = generic_clone_url(f, owner, repo);
    if (!url)
        return -1;
    int rc = gf_git_ls_remote(url, branch, sha_out, f->token);
    free(url);
    return rc == 0 ? 0 : 1;
}

static char *gen_tarball_url(gf_forge *f, const char *owner, const char *repo,
                             const char *tag)
{
    /* No tarball service: source acquisition falls back to git extraction. */
    (void)f;
    (void)owner;
    (void)repo;
    (void)tag;
    return NULL;
}

const struct gf_forge_ops gf_forge_ops_generic = {
    .kind = "generic",
    .repo_info = gen_repo_info,
    .list_releases = gen_list_releases,
    .list_tags = gen_list_tags,
    .branch_head = gen_branch_head,
    .tarball_url = gen_tarball_url,
    .release_assets = NULL,
    .search = NULL,
};
