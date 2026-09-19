/* gitx.c — Git plumbing through controlled child processes. */
#include "gitx.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GIT_SHA_LEN 40

int gf_git_check(char *version, size_t versionlen)
{
    const char *argv[] = { "git", "--version", NULL };
    gf_strbuf out;
    gf_strbuf_init(&out);
    gf_exec_opts eo = gf_exec_opts_default();
    eo.out = &out;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    if (gf_exec_capture(argv, &eo, &res) != 0 || res.status != 0) {
        gf_strbuf_free(&out);
        return -1;
    }
    char *v = gf_strbuf_steal(&out);
    if (!gf_str_starts_with(v, "git version "))
        v[strlen("git version ")] = '\0';
    if (version)
        snprintf(version, versionlen, "%s", v + strlen("git version "));
    free(v);
    return 0;
}

/* ---- ephemeral credential helper ------------------------------------- */

typedef struct gf_askpass {
    char *script_path;
    char *token_path;
} gf_askpass;

/* Create an askpass script + token file (0600) for a git child. */
static int askpass_setup(gf_askpass *ap, const char *token)
{
    memset(ap, 0, sizeof(*ap));
    if (!token || !*token)
        return 0;
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";
    char *script = NULL, *tok = NULL;
    int sfd = gf_mkstemp_in(tmpdir, &script);
    if (sfd < 0)
        return -1;
    int tfd = gf_mkstemp_in(tmpdir, &tok);
    if (tfd < 0) {
        close(sfd);
        unlink(script);
        free(script);
        return -1;
    }
    if (write(sfd, token, strlen(token)) != (ssize_t)strlen(token)) {
        close(sfd);
        close(tfd);
        unlink(script);
        unlink(tok);
        free(script);
        free(tok);
        return -1;
    }
    close(tfd);
    fchmod(sfd, 0700);
    chmod(tok, 0600);
    char body[512];
    snprintf(body, sizeof(body),
             "#!/bin/sh\ncase \"$1\" in\n"
             "  Username*) printf 'gitfull' ;;\n"
             "  Password*) cat '%s' ;;\n"
             "  *) printf '' ;;\nesac\n",
             tok);
    if (write(sfd, body, strlen(body)) != (ssize_t)strlen(body)) {
        close(sfd);
        unlink(tok);
        free(script);
        free(tok);
        return -1;
    }
    close(sfd);
    ap->script_path = script;
    ap->token_path = tok;
    return 0;
}

static void askpass_teardown(gf_askpass *ap)
{
    if (ap->script_path) {
        unlink(ap->script_path);
        free(ap->script_path);
    }
    if (ap->token_path) {
        unlink(ap->token_path);
        free(ap->token_path);
    }
    memset(ap, 0, sizeof(*ap));
}

/* Writable env strings (arrays, not literals) so execvpe gets char*const*
 * without discarding const. */
static char EV_PROMPT[] = "GIT_TERMINAL_PROMPT=0";
static char EV_NOSYSTEM[] = "GIT_CONFIG_NOSYSTEM=1";
static char EV_NOGLOBAL[] = "GIT_CONFIG_GLOBAL=/dev/null";
static char EV_PROTO[] = "GIT_ALLOW_PROTOCOL=https";
static char EV_LC[] = "LC_ALL=C";
static char EV_TZ[] = "TZ=UTC";

/* Run git with our environment; returns 0 when exit status == 0. */
static int git_run(const char *const *argv, const char *cwd, gf_strbuf *out,
                   gf_strbuf *err)
{
    char *envvars[8] = {
        EV_PROMPT, EV_NOSYSTEM, EV_NOGLOBAL, EV_PROTO, EV_LC, EV_TZ, NULL, NULL
    };
    gf_exec_opts eo = gf_exec_opts_default();
    eo.envp = envvars;
    eo.cwd = cwd;
    eo.out = out;
    eo.err = err;
    eo.timeout_ms = 15u * 60u * 1000u;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    int rc = gf_exec_capture(argv, &eo, &res);
    if (rc != 0)
        return -1;
    if (res.signaled || res.timed_out)
        return -1;
    return res.status == 0 ? 0 : -1;
}

/* Like git_run but with token askpass support. */
static int git_run_token(const char *const *argv, const char *cwd,
                         const char *token, gf_strbuf *out, gf_strbuf *err)
{
    gf_askpass ap;
    if (askpass_setup(&ap, token) != 0)
        return -1;
    char *envvars[8] = {
        EV_PROMPT, EV_NOSYSTEM, EV_NOGLOBAL, EV_PROTO, EV_LC, EV_TZ, NULL, NULL
    };
    char askpass_var[512];
    if (ap.script_path) {
        snprintf(askpass_var, sizeof(askpass_var), "GIT_ASKPASS=%s",
                 ap.script_path);
        envvars[6] = askpass_var;
    }
    gf_exec_opts eo = gf_exec_opts_default();
    eo.envp = envvars;
    eo.cwd = cwd;
    eo.out = out;
    eo.err = err;
    eo.timeout_ms = 15u * 60u * 1000u;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    int rc = gf_exec_capture(argv, &eo, &res);
    askpass_teardown(&ap);
    if (rc != 0)
        return -1;
    if (res.signaled || res.timed_out)
        return -1;
    return res.status == 0 ? 0 : -1;
}

int gf_git_ls_remote(const char *url, const char *ref, char *sha_out,
                     const char *token)
{
    sha_out[0] = '\0';
    char refspec[512];
    snprintf(refspec, sizeof(refspec), "refs/tags/%s", ref);
    const char *try_refs[3] = { refspec, NULL, NULL };
    char branchspec[512];
    snprintf(branchspec, sizeof(branchspec), "refs/heads/%s", ref);
    try_refs[1] = branchspec;
    try_refs[2] = ref; /* raw (HEAD, refs/...) */

    for (int i = 0; i < 3; i++) {
        const char *argv[] = { "git", "ls-remote", "--", url, try_refs[i], NULL };
        gf_strbuf out;
        gf_strbuf_init(&out);
        gf_strbuf err;
        gf_strbuf_init(&err);
        int rc = git_run_token(argv, NULL, token, &out, &err);
        if (rc == 0) {
            char *line = gf_strbuf_steal(&out);
            /* "<40-hex>\trefs/..." possibly multiple lines (tag + tag^{}) */
            char *sp = strchr(line, '\t');
            if (sp && strlen(line) >= GIT_SHA_LEN) {
                /* annotated tags come with two lines; prefer peeled ^{} */
                char *nl = strchr(line, '\n');
                if (nl && strstr(nl, "^{}")) {
                    memcpy(sha_out, nl + 1, GIT_SHA_LEN);
                    sha_out[GIT_SHA_LEN] = '\0';
                } else {
                    memcpy(sha_out, line, GIT_SHA_LEN);
                    sha_out[GIT_SHA_LEN] = '\0';
                }
                free(line);
                gf_strbuf_free(&err);
                return 0;
            }
            free(line);
        }
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
    }
    return -1;
}

int gf_git_ls_remote_head(const char *url, char **branch_out, char *sha_out,
                          const char *token)
{
    *branch_out = NULL;
    const char *argv[] = { "git", "ls-remote", "--symref", "--", url, "HEAD", NULL };
    gf_strbuf out, err;
    gf_strbuf_init(&out);
    gf_strbuf_init(&err);
    int rc = git_run_token(argv, NULL, token, &out, &err);
    if (rc != 0) {
        gf_log(GF_LOG_ERROR, "git ls-remote failed for %s", url);
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        return -1;
    }
    char *text = gf_strbuf_steal(&out);
    char *save = NULL;
    char *branch = NULL;
    char sha[GIT_SHA_LEN + 1] = "";
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (gf_str_starts_with(line, "ref: ")) {
            char *ref = line + 5;
            char *sp = strchr(ref, '\t');
            if (sp)
                *sp = '\0';
            free(branch);
            branch = gf_strdup(ref + strlen("refs/heads/"));
        } else if (strlen(line) >= GIT_SHA_LEN) {
            memcpy(sha, line, GIT_SHA_LEN);
            sha[GIT_SHA_LEN] = '\0';
        }
    }
    free(text);
    gf_strbuf_free(&err);
    if (!branch || !sha[0]) {
        free(branch);
        return -1;
    }
    *branch_out = branch;
    strcpy(sha_out, sha);
    return 0;
}

char *gf_git_cache_path(const char *state_dir, const char *url)
{
    /* stable, filesystem-safe cache name from the URL */
    char *norm = gf_strdup(url);
    for (char *p = norm; *p; p++) {
        if (*p == '/' || *p == ':' || *p == '@' || *p == '?')
            *p = '_';
    }
    char *name = norm;
    /* skip leading https___ (was https://) */
    if (gf_str_starts_with(name, "https___"))
        name += 8;
    char *dir = gf_path_join_multi(state_dir, "cache", "git", name, NULL);
    free(norm);
    return dir;
}

int gf_git_cache_fetch(const char *mirror_path, const char *url,
                       const char *token)
{
    char *parent = gf_path_dirname(mirror_path);
    if (gf_fs_mkdir_p_soft(parent) != 0) {
        /* cache unavailable (e.g. read-only state dir): not fatal, the
         * caller degrades verification; keep it quiet but explain once */
        gf_log(GF_LOG_DEBUG, "git mirror cache unavailable: %s", parent);
        free(parent);
        return -1;
    }
    free(parent);

    if (!gf_fs_is_dir(mirror_path)) {
        const char *argv[] = { "git", "clone", "--bare", "--no-checkout", "--",
                               url, mirror_path, NULL };
        gf_strbuf err;
        gf_strbuf_init(&err);
        int rc = git_run_token(argv, NULL, token, NULL, &err);
        if (rc != 0) {
            gf_log(GF_LOG_ERROR, "git clone failed for %s: %s", url,
                   gf_strbuf_str(&err));
            gf_strbuf_free(&err);
            gf_fs_rm_rf(mirror_path);
            return -1;
        }
        gf_strbuf_free(&err);
        return 0;
    }
    /* fetch updates: tags + heads */
    const char *argv[] = { "git", "--git-dir", mirror_path, "fetch", "--tags",
                           "--prune", "origin", "+refs/heads/*:refs/heads/*", NULL };
    gf_strbuf err;
    gf_strbuf_init(&err);
    int rc = git_run_token(argv, NULL, token, NULL, &err);
    if (rc != 0) {
        gf_log(GF_LOG_ERROR, "git fetch failed for %s: %s", url,
               gf_strbuf_str(&err));
        gf_strbuf_free(&err);
        return -1;
    }
    gf_strbuf_free(&err);
    return 0;
}

int gf_git_cache_extract_tree(const char *mirror_path, const char *sha,
                              const char *dest_dir)
{
    /* Verify the sha exists in the mirror and looks like a commit. */
    {
        const char *argv[] = { "git", "--git-dir", mirror_path, "cat-file",
                               "-e", sha, NULL };
        if (git_run(argv, NULL, NULL, NULL) != 0) {
            gf_log(GF_LOG_ERROR, "commit %s not present in cache", sha);
            return -1;
        }
    }
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";
    char idxpath[512];
    snprintf(idxpath, sizeof(idxpath), "%s/gitfull-idx.XXXXXX", tmpdir);
    int ifd = mkstemp(idxpath);
    if (ifd < 0)
        return -1;
    close(ifd);
    unlink(idxpath);

    char envidx[1024];
    snprintf(envidx, sizeof(envidx), "GIT_INDEX_FILE=%s", idxpath);
    char *envvars[8] = { envidx, EV_PROMPT, EV_NOSYSTEM, EV_NOGLOBAL,
                         EV_LC, EV_TZ, NULL, NULL };

    int rc = -1;
    do {
        /* 1. read the tree */
        const char *read_argv[] = { "git", "--git-dir", mirror_path,
                                    "read-tree", sha, NULL };
        gf_exec_opts eo = gf_exec_opts_default();
        eo.envp = envvars;
        gf_exec_result res;
        memset(&res, 0, sizeof(res));
        if (gf_exec_capture(read_argv, &eo, &res) != 0 || res.status != 0)
            break;

        /* 2. checkout-index into dest (prefix must end with '/') */
        char prefix[4096];
        if (snprintf(prefix, sizeof(prefix), "%s/", dest_dir) >= (int)sizeof(prefix))
            break;
        const char *co_argv[] = { "git", "--git-dir", mirror_path,
                                  "checkout-index", "-a", "-f",
                                  "--prefix", prefix, NULL };
        gf_exec_opts eo2 = gf_exec_opts_default();
        eo2.envp = envvars;
        gf_exec_result res2;
        memset(&res2, 0, sizeof(res2));
        if (gf_exec_capture(co_argv, &eo2, &res) != 0 || res2.status != 0)
            break;
        rc = 0;
    } while (0);

    unlink(idxpath);
    return rc;
}

int gf_git_rev_parse(const char *repo_dir, const char *rev, char *sha_out)
{
    char gitdir[4096];
    snprintf(gitdir, sizeof(gitdir), "%s/.git", repo_dir);
    const char *full[] = { "git", "--git-dir", gitdir, "rev-parse",
                           "--verify", rev, NULL };
    gf_strbuf out, err;
    gf_strbuf_init(&out);
    gf_strbuf_init(&err);
    int rc = git_run(full, NULL, &out, &err);
    if (rc != 0) {
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        return -1;
    }
    gf_strbuf_free(&err);
    char *sha = gf_strbuf_steal(&out);
    gf_str_trim(sha);
    if (strlen(sha) < GIT_SHA_LEN) {
        free(sha);
        return -1;
    }
    memcpy(sha_out, sha, GIT_SHA_LEN);
    sha_out[GIT_SHA_LEN] = '\0';
    free(sha);
    return 0;
}

int gf_git_list_tags(const char *repo_dir, char ***out, size_t *count)
{
    *out = NULL;
    *count = 0;
    char gitdir[4096];
    snprintf(gitdir, sizeof(gitdir), "%s/.git", repo_dir);
    const char *full[] = { "git", "--git-dir", gitdir, "for-each-ref",
                           "--format=%(refname:short)", "refs/tags", NULL };
    gf_strbuf outb, err;
    gf_strbuf_init(&outb);
    gf_strbuf_init(&err);
    int rc = git_run(full, NULL, &outb, &err);
    if (rc != 0) {
        gf_strbuf_free(&outb);
        gf_strbuf_free(&err);
        return -1;
    }
    gf_strbuf_free(&err);
    char *text = gf_strbuf_steal(&outb);
    /* split on newlines; empty lines dropped */
    char **tags = NULL;
    size_t n = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *t = gf_str_trim(line);
        if (!*t)
            continue;
        tags = gf_realloc(tags, (n + 1) * sizeof(char *));
        tags[n++] = gf_strdup(t);
    }
    free(text);
    *out = tags;
    *count = n;
    return 0;
}

int gf_git_verify_tag(const char *repo_dir, const char *tag)
{
    /* gpg presence check */
    const char *gpg[] = { "gpgv", "--version", NULL };
    gf_exec_result r;
    memset(&r, 0, sizeof(r));
    gf_exec_opts eo = gf_exec_opts_default();
    if (gf_exec_capture(gpg, &eo, &r) != 0 || r.status != 0) {
        return -1; /* cannot check */
    }
    char gitdir[4096];
    snprintf(gitdir, sizeof(gitdir), "%s/.git", repo_dir);
    const char *full[] = { "git", "--git-dir", gitdir, "verify-tag", tag, NULL };
    gf_strbuf out, err;
    gf_strbuf_init(&out);
    gf_strbuf_init(&err);
    int rc = git_run(full, NULL, &out, &err);
    gf_strbuf_free(&out);
    gf_strbuf_free(&err);
    if (rc == 0)
        return 0;  /* signed and valid */
    /* distinguish unsigned vs bad signature: check stderr text */
    return 1;
}

int gf_git_clone_exact(const char *url, const char *sha, const char *dest,
                       const char *token)
{
    if (gf_fs_mkdir_p(dest) != 0)
        return -1;
    const char *argv[] = { "git", "clone", "--no-checkout", "--", url, dest, NULL };
    gf_strbuf err;
    gf_strbuf_init(&err);
    int rc = git_run_token(argv, NULL, token, NULL, &err);
    if (rc != 0) {
        gf_log(GF_LOG_ERROR, "git clone failed: %s", gf_strbuf_str(&err));
        gf_strbuf_free(&err);
        return -1;
    }
    gf_strbuf_free(&err);
    const char *co[] = { "git", "--git-dir", NULL, "checkout", "--detach", NULL, NULL };
    char gitdir[4096];
    snprintf(gitdir, sizeof(gitdir), "%s/.git", dest);
    const char *full[] = { "git", "--git-dir", gitdir, "checkout", "--detach",
                           sha, "--", NULL };
    (void)co;
    gf_strbuf err2;
    gf_strbuf_init(&err2);
    rc = git_run(full, dest, NULL, &err2);
    if (rc != 0) {
        gf_log(GF_LOG_ERROR, "git checkout %s failed: %s", sha,
               gf_strbuf_str(&err2));
        gf_strbuf_free(&err2);
        return -1;
    }
    gf_strbuf_free(&err2);
    return 0;
}
