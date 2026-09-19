/* source.c — forge resolution + source acquisition + verification. */
#include "source.h"

#include "common.h"
#include "gitx.h"
#include "http.h"
#include "sha256.h"
#include "store.h"
#include "tar.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *gf_src_status_name(gf_src_status s)
{
    switch (s) {
    case GF_SRCST_VERIFIED:   return "verified";
    case GF_SRCST_CHECKSUM:   return "checksum";
    case GF_SRCST_UNSIGNED:   return "unsigned";
    case GF_SRCST_UNVERIFIED: return "unverified";
    case GF_SRCST_EDGE:       return "edge";
    default:                  return "unknown";
    }
}

void gf_source_id_free(gf_source_id *s)
{
    if (!s)
        return;
    free(s->forge);
    free(s->repo_url);
    free(s->owner);
    free(s->repo);
    free(s->ref);
    free(s->commit);
    free(s->tree);
    free(s->archive_sha);
    free(s->description);
    free(s->license);
    memset(s, 0, sizeof(*s));
}

gf_source_id *gf_source_id_copy(const gf_source_id *s)
{
    if (!s)
        return NULL;
    gf_source_id *c = gf_memdup(s, sizeof(*s));
    c->forge = s->forge ? gf_strdup(s->forge) : NULL;
    c->repo_url = s->repo_url ? gf_strdup(s->repo_url) : NULL;
    c->owner = s->owner ? gf_strdup(s->owner) : NULL;
    c->repo = s->repo ? gf_strdup(s->repo) : NULL;
    c->ref = s->ref ? gf_strdup(s->ref) : NULL;
    c->commit = s->commit ? gf_strdup(s->commit) : NULL;
    c->tree = s->tree ? gf_strdup(s->tree) : NULL;
    c->archive_sha = s->archive_sha ? gf_strdup(s->archive_sha) : NULL;
    c->description = s->description ? gf_strdup(s->description) : NULL;
    c->license = s->license ? gf_strdup(s->license) : NULL;
    return c;
}

void gf_resolved_free(gf_resolved *r)
{
    if (!r)
        return;
    gf_pkgref_free(&r->ref);
    gf_source_id_free(&r->src);
    free(r->version);
    free(r);
}

/* find the repository config for a ref: explicit forge name, host match, or
 * the configured default */
static const gf_repo *repo_for_ref(const gf_config *cfg, const gf_pkgref *ref)
{
    if (ref->forge) {
        const gf_repo *r = gf_config_repo(cfg, ref->forge);
        if (r)
            return r;
        gf_log(GF_LOG_ERROR, "unknown repository: %s", ref->forge);
        return NULL;
    }
    if (ref->host) {
        const gf_repo *r = gf_config_repo_by_host(cfg, ref->host);
        if (r)
            return r;
    }
    return gf_config_repo(cfg, cfg->default_repo);
}

/* does the tag have a release asset digest? (github asset digests) */
static char *asset_digest_for(const gf_json *assets)
{
    if (!assets || assets->type != GF_JSON_ARRAY)
        return NULL;
    size_t n = gf_json_len(assets);
    for (size_t i = 0; i < n; i++) {
        const gf_json *a = gf_json_at(assets, i);
        if (!a)
            continue;
        const char *digest = gf_json_str(gf_json_get(a, "digest"));
        if (digest && gf_str_starts_with(digest, "sha256:"))
            return gf_strdup(digest + 7);
    }
    return NULL;
}

gf_resolved *gf_resolve(const gf_config *cfg, const gf_pkgref *ref,
                        const gf_resolve_opts *opts)
{
    if (ref->local) {
        gf_resolved *r = gf_calloc(1, sizeof(gf_resolved));
        r->ref = gf_pkgref_copy(ref);
        r->src.forge = gf_strdup("local");
        r->src.repo_url = gf_strdup(ref->local_path);
        r->src.status = GF_SRCST_UNSIGNED;
        gf_resolve_opts defopts = { 0 };
        const gf_resolve_opts *o = opts ? opts : &defopts;
        const char *want = (ref->version && *ref->version) ? ref->version
                                                            : NULL;
        bool is_git = gf_fs_is_dir(gf_path_join(ref->local_path, ".git"));

        if (is_git && o->edge && !want) {
            /* EDGE: head of the local default branch, same policy as remote */
            char sha[41];
            if (gf_git_rev_parse(ref->local_path, "HEAD", sha) == 0) {
                r->is_edge = true;
                r->src.ref = gf_strdup("HEAD");
                r->src.commit = gf_strdup(sha);
                r->src.status = GF_SRCST_EDGE;
                r->version = gf_strdup("edge");
            }
        } else if (is_git && want) {
            /* exact tag pinned via @version */
            char tagref[300];
            char sha[41];
            snprintf(tagref, sizeof(tagref), "refs/tags/%s", want);
            if (gf_git_rev_parse(ref->local_path, tagref, sha) == 0) {
                r->src.ref = gf_strdup(want);
                r->version = gf_strdup(want);
                r->src.commit = gf_strdup(sha);
            } else {
                gf_log(GF_LOG_ERROR, "version '%s' not found in %s", want,
                       ref->local_path);
                gf_resolved_free(r);
                return NULL;
            }
        } else if (is_git) {
            /* STABLE default: newest stable release tag, same policy as the
             * forge release selection (prereleases skipped unless allowed). */
            char **tags = NULL;
            size_t ntags = 0;
            if (gf_git_list_tags(ref->local_path, &tags, &ntags) == 0) {
                char *best = gf_version_best_tag(tags, ntags,
                                                 o->prerelease);
                if (best) {
                    char tagref[300];
                    char sha[41];
                    snprintf(tagref, sizeof(tagref), "refs/tags/%s", best);
                    if (gf_git_rev_parse(ref->local_path, tagref, sha) == 0) {
                        r->src.ref = best;
                        r->version = gf_strdup(best);
                        r->src.commit = gf_strdup(sha);
                    } else {
                        free(best);
                    }
                }
                for (size_t i = 0; i < ntags; i++)
                    free(tags[i]);
                free(tags);
            }
            if (!r->src.commit) {
                /* no usable release tags: HEAD with an explicit notice */
                gf_log(GF_LOG_INFO,
                       "no stable release tags in %s; using HEAD "
                       "(pin @version or --edge to be explicit)",
                       ref->local_path);
                char sha[41];
                if (gf_git_rev_parse(ref->local_path, "HEAD", sha) == 0) {
                    r->src.ref = gf_strdup("HEAD");
                    r->src.commit = gf_strdup(sha);
                    r->version = gf_strdup("HEAD");
                }
            }
        }
        if (!r->src.commit) {
            /* plain directory (or a git repo with no HEAD): no git identity */
            r->src.ref = gf_strdup(want ? want : "HEAD");
            r->src.commit = gf_strdup("no-git");
            r->version = gf_strdup(want ? want : "HEAD");
        }
        return r;
    }

    const gf_repo *repo = repo_for_ref(cfg, ref);
    if (!repo) {
        gf_log(GF_LOG_ERROR, "no repository configuration matches this "
                             "package reference");
        return NULL;
    }
    gf_forge *f = gf_forge_open(repo, cfg);
    if (!f)
        return NULL;

    gf_resolved *r = gf_calloc(1, sizeof(gf_resolved));
    r->ref = gf_pkgref_copy(ref);
    r->src.forge = gf_strdup(repo->name);
    char *web = gf_repo_web_base(repo);
    r->src.repo_url = gf_pkgref_clone_url(ref, web);
    free(web);
    r->src.owner = ref->owner ? gf_strdup(ref->owner) : NULL;
    r->src.repo = ref->repo ? gf_strdup(ref->repo) : NULL;

    /* repo info: default branch + license/description */
    char *default_branch = NULL, *description = NULL, *license = NULL;
    int irc = f->ops->repo_info(f, ref->owner, ref->repo, &default_branch,
                                &description, &license);
    if (irc != 0 && !(ref->version || opts->exact_tag)) {
        gf_log(GF_LOG_ERROR, "repository %s/%s not found on %s",
               ref->owner ? ref->owner : "", ref->repo ? ref->repo : "",
               repo->name);
        gf_forge_close(f);
        free(default_branch);
        free(description);
        free(license);
        gf_resolved_free(r);
        return NULL;
    }
    r->src.description = description;
    r->src.license = license;

    gf_resolve_opts defopts = { 0 };
    const gf_resolve_opts *o = opts ? opts : &defopts;

    if (o->edge) {
        /* EDGE: head of the default branch */
        r->is_edge = true;
        char sha[41];
        if (!default_branch)
            default_branch = gf_strdup("main");
        if (f->ops->branch_head(f, ref->owner, ref->repo, default_branch,
                                sha) != 0) {
            /* fallback: git ls-remote */
            if (gf_git_ls_remote(r->src.repo_url, default_branch, sha,
                                 gf_repo_token(repo)) != 0) {
                gf_log(GF_LOG_ERROR, "cannot resolve branch %s on %s",
                       default_branch, r->src.repo_url);
                gf_forge_close(f);
                free(default_branch);
                gf_resolved_free(r);
                return NULL;
            }
        }
        r->src.ref = default_branch;
        r->src.commit = gf_strdup(sha);
        r->src.status = GF_SRCST_EDGE;
        r->version = gf_strdup("edge");
        gf_forge_close(f);
        return r;
    }

    /* stable or exact */
    const char *want_tag = NULL;
    if (o->exact_tag)
        want_tag = o->exact_tag;
    else if (ref->version && *ref->version)
        want_tag = ref->version;

    gf_json *releases_json = NULL;
    gf_rel *rels = NULL;
    size_t nrels = 0;
    if (f->ops->list_releases) {
        if (f->ops->list_releases(f, ref->owner, ref->repo, &releases_json) == 0) {
            gf_forge_rels_from_json(releases_json, &rels, &nrels);
        }
    }

    const gf_rel *chosen = NULL;
    if (want_tag) {
        chosen = gf_forge_find_release(rels, nrels, want_tag);
        if (!chosen) {
            /* tags list may know it even without a release */
            gf_json *tags_json = NULL;
            if (f->ops->list_tags(f, ref->owner, ref->repo, &tags_json) == 0) {
                gf_rel *trels = NULL;
                size_t nt = 0;
                gf_forge_rels_from_json(tags_json, &trels, &nt);
                chosen = gf_forge_find_release(trels, nt, want_tag);
                if (chosen) {
                    /* copy into a persistent rel */
                    gf_rel *c = gf_memdup(chosen, sizeof(gf_rel));
                    c->tag = gf_strdup(chosen->tag);
                    c->published_at = chosen->published_at
                        ? gf_strdup(chosen->published_at) : NULL;
                    c->commit = chosen->commit ? gf_strdup(chosen->commit) : NULL;
                    c->assets = gf_json_new_array();
                    /* extend the rels array */
                    rels = gf_realloc(rels, (nrels + 1) * sizeof(gf_rel));
                    rels[nrels] = *c;
                    chosen = &rels[nrels];
                    nrels++;
                }
                gf_forge_rels_free(trels, nt);
                gf_json_free(tags_json);
            }
        }
        if (!chosen) {
            gf_log(GF_LOG_ERROR, "version '%s' not found for %s/%s",
                   want_tag, ref->owner ? ref->owner : "", ref->repo);
            goto fail;
        }
    } else {
        if (gf_forge_select_release(rels, nrels, o->prerelease, &chosen) != 0) {
            gf_log(GF_LOG_ERROR,
                   "no stable release found for %s/%s (try --edge or an "
                   "exact @version)",
                   ref->owner ? ref->owner : "", ref->repo);
            goto fail;
        }
    }

    r->src.ref = gf_strdup(chosen->tag);
    r->version = gf_strdup(chosen->tag);
    if (chosen->commit)
        r->src.commit = gf_strdup(chosen->commit);

    /* resolve the exact commit through the git mirror (authoritative) */
    {
        char *mirror = gf_git_cache_path(cfg->state_dir, r->src.repo_url);
        char *token = gf_repo_token(repo);
        if (gf_git_cache_fetch(mirror, r->src.repo_url, token) == 0) {
            char refspec[256];
            snprintf(refspec, sizeof(refspec), "refs/tags/%s", chosen->tag);
            const char *argv[] = { "git", "--git-dir", mirror, "rev-parse",
                                   "--verify", refspec, NULL };
            gf_strbuf out;
            gf_strbuf_init(&out);
            gf_exec_opts eo = gf_exec_opts_default();
            eo.out = &out;
            gf_exec_result res;
            memset(&res, 0, sizeof(res));
            char *envvars[2] = { NULL, NULL };
            (void)envvars;
            if (gf_exec_capture(argv, &eo, &res) == 0 && res.status == 0) {
                char *sha = gf_strbuf_steal(&out);
                gf_str_trim(sha);
                free(r->src.commit);
                r->src.commit = sha; /* tag object sha */
                /* dereference annotated tag to commit */
                const char *deref[] = { "git", "--git-dir", mirror,
                                        "rev-parse", "--verify",
                                        refspec, "^{commit}", NULL };
                gf_strbuf out2;
                gf_strbuf_init(&out2);
                gf_exec_opts eo2 = gf_exec_opts_default();
                eo2.out = &out2;
                gf_exec_result res2;
                memset(&res2, 0, sizeof(res2));
                if (gf_exec_capture(deref, &eo2, &res2) == 0 &&
                    res2.status == 0) {
                    char *csha = gf_strbuf_steal(&out2);
                    gf_str_trim(csha);
                    if (strlen(csha) >= 40) {
                        free(r->src.commit);
                        r->src.commit = csha;
                    } else {
                        free(csha);
                    }
                }
            } else {
                gf_strbuf_free(&out);
            }
        }
        free(mirror);
        free(token);
    }

    /* verification status: asset digest or tag signature */
    char *digest = asset_digest_for(chosen->assets);
    bool signed_valid = false, cannot_check = true;
    gf_source_verify_signature(cfg, r, &signed_valid, &cannot_check);
    if (signed_valid)
        r->src.status = GF_SRCST_VERIFIED;
    else if (digest)
        r->src.status = GF_SRCST_CHECKSUM; /* digest verified at download */
    else if (!cannot_check)
        r->src.status = GF_SRCST_UNSIGNED; /* checked, no signature */
    else
        r->src.status = GF_SRCST_UNVERIFIED;
    r->src.archive_sha = digest; /* digest consumed at download time */

    gf_json_free(releases_json);
    gf_forge_rels_free(rels, nrels);
    gf_forge_close(f);
    free(default_branch);
    return r;

fail:
    gf_json_free(releases_json);
    gf_forge_rels_free(rels, nrels);
    gf_forge_close(f);
    free(default_branch);
    gf_resolved_free(r);
    return NULL;
}

int gf_source_verify_signature(const gf_config *cfg, const gf_resolved *r,
                               bool *signed_valid, bool *cannot_check)
{
    *signed_valid = false;
    *cannot_check = true;
    (void)cfg;
    if (!r->src.repo_url || !r->src.ref || r->is_edge || r->ref.local)
        return 0;
    /* verify the tag signature in a git mirror */
    char *mirror = gf_git_cache_path(cfg->state_dir, r->src.repo_url);
    if (!gf_fs_is_dir(mirror)) {
        free(mirror);
        return 0; /* nothing to verify against yet */
    }
    char refspec[256];
    snprintf(refspec, sizeof(refspec), "refs/tags/%s", r->src.ref);
    const char *argv[] = { "git", "--git-dir", mirror, "verify-tag",
                           refspec, NULL };
    gf_strbuf out, err;
    gf_strbuf_init(&out);
    gf_strbuf_init(&err);
    gf_exec_opts eo = gf_exec_opts_default();
    eo.err = &err;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    if (gf_exec_capture(argv, &eo, &res) != 0) {
        gf_strbuf_free(&out);
        gf_strbuf_free(&err);
        free(mirror);
        return -1;
    }
    *cannot_check = false;
    *signed_valid = res.status == 0;
    gf_strbuf_free(&out);
    gf_strbuf_free(&err);
    free(mirror);
    return 0;
}

int gf_source_acquire(const gf_config *cfg, gf_resolved *r,
                      const char *dest_dir)
{
    if (gf_fs_exists(dest_dir)) {
        gf_log(GF_LOG_ERROR, "source destination exists: %s", dest_dir);
        return -1;
    }
    if (gf_fs_mkdir_p(dest_dir) != 0)
        return -1;

    if (r->ref.local) {
        /* local checkout: extract the EXACT tree (tag commit when pinned,
         * HEAD otherwise). Falls back to a plain copy for non-git dirs. */
        char *gitdir = gf_path_join(r->ref.local_path, ".git");
        bool is_git = gf_fs_is_dir(gitdir);
        free(gitdir);
        if (is_git && r->src.commit && strcmp(r->src.commit, "no-git") != 0) {
            /* read-tree + checkout-index with a temp index */
            char idxpath[512];
            snprintf(idxpath, sizeof(idxpath), "%s/../gitfull-local-idx.XXXXXX",
                     dest_dir);
            int ifd = mkstemp(idxpath);
            if (ifd < 0)
                return -1;
            close(ifd);
            unlink(idxpath);
            char gitdirbuf[4096];
            snprintf(gitdirbuf, sizeof(gitdirbuf), "%s/.git", r->ref.local_path);
            char envidx[2048];
            snprintf(envidx, sizeof(envidx), "GIT_INDEX_FILE=%s", idxpath);
            static char EV1[] = "GIT_TERMINAL_PROMPT=0";
            static char EV2[] = "GIT_CONFIG_NOSYSTEM=1";
            static char EV3[] = "GIT_CONFIG_GLOBAL=/dev/null";
            static char EV4[] = "LC_ALL=C";
            static char EV5[] = "TZ=UTC";
            char *envvars[8] = { envidx, EV1, EV2, EV3, EV4, EV5, NULL, NULL };
            int rc = -1;
            do {
                char commitref[128];
                snprintf(commitref, sizeof(commitref), "%s^{commit}",
                         r->src.commit);
                const char *rt[] = { "git", "--git-dir", gitdirbuf,
                                     "read-tree", commitref, NULL };
                gf_exec_opts eo = gf_exec_opts_default();
                eo.envp = envvars;
                gf_exec_result res;
                memset(&res, 0, sizeof(res));
                if (gf_exec_capture(rt, &eo, &res) != 0 || res.status != 0)
                    break;
                char prefix[4096];
                snprintf(prefix, sizeof(prefix), "%s/", dest_dir);
                const char *co[] = { "git", "--git-dir", gitdirbuf,
                                     "checkout-index", "-a", "-f", "--prefix",
                                     prefix, NULL };
                gf_exec_opts eo2 = gf_exec_opts_default();
                eo2.envp = envvars;
                gf_exec_result res2;
                memset(&res2, 0, sizeof(res2));
                if (gf_exec_capture(co, &eo2, &res2) != 0 || res2.status != 0)
                    break;
                rc = 0;
            } while (0);
            unlink(idxpath);
            if (rc == 0)
                return 0;
        }
        /* plain directory (or git plumbing failed): copy without .git */
        return gf_fs_copy_tree_skip_git(r->ref.local_path, dest_dir);
    }

    /* try the forge tarball first */
    const gf_repo *repo = gf_config_repo(cfg, r->src.forge);
    gf_forge *f = repo ? gf_forge_open(repo, cfg) : NULL;
    if (f && f->ops->tarball_url) {
        char *url = f->ops->tarball_url(f, r->src.owner, r->src.repo, r->src.ref);
        gf_forge_close(f);
        if (url) {
            /* cache tarballs by URL hash under <state>/cache/tarballs */
            char *cache_dir = gf_path_join(cfg->state_dir, "cache/tarballs");
            gf_fs_mkdir_p_soft(cache_dir);
            char *urlhash = gf_sha256_buf_hex(url, strlen(url));
            char *cached = gf_path_join(cache_dir, urlhash);
            free(urlhash);
            free(cache_dir);

            gf_http_opts ho = gf_http_opts_default();
            ho.timeout_secs = cfg->timeout_secs;
            ho.retries = cfg->retries;
            ho.max_bytes = (size_t)cfg->max_download_mb * 1024u * 1024u;

            if (!gf_fs_is_file(cached)) {
                gf_log(GF_LOG_INFO, "downloading %s", url);
                int code = gf_http_download(url, cached, NULL, &ho);
                if (code < 200 || code >= 300) {
                    free(cached);
                    free(url);
                    /* fall through to git extraction */
                    goto git_fallback;
                }
                char *sum = gf_sha256_file_hex(cached);
                if (sum) {
                    gf_log(GF_LOG_INFO, "archive sha256: %s", sum);
                    if (!r->src.archive_sha) {
                        r->src.archive_sha = sum; /* record identity */
                    } else if (strcmp(sum, r->src.archive_sha) != 0) {
                        /* digest provided by the forge: verify */
                        gf_log(GF_LOG_ERROR,
                               "archive checksum mismatch: expected %s got %s",
                               r->src.archive_sha, sum);
                        free(sum);
                        free(cached);
                        free(url);
                        return -1;
                    } else {
                        free(sum);
                    }
                }
            }
            free(url);
            /* extract with hardened tar reader (auto-strip top dir) */
            gf_tar_limits lim = gf_tar_limits_default();
            lim.max_total_bytes = (uint64_t)(unsigned)cfg->max_download_mb * 1024ull *
                                  1024ull;
            if (gf_tar_extract(cached, dest_dir, -1, &lim, NULL) != 0) {
                gf_log(GF_LOG_ERROR, "archive extraction failed: %s", cached);
                free(cached);
                return -1;
            }
            free(cached);
            return 0;
        }
    } else if (f) {
        gf_forge_close(f);
    }

git_fallback:
    /* git mirror extraction at the exact commit */
    {
        char *mirror = gf_git_cache_path(cfg->state_dir, r->src.repo_url);
        char *token = repo ? gf_repo_token(repo) : NULL;
        if (gf_git_cache_fetch(mirror, r->src.repo_url, token) != 0) {
            free(mirror);
            free(token);
            return -1;
        }
        free(token);
        /* ensure the tag ref exists in the mirror (already fetched tags) */
        if (!r->src.commit || !r->src.commit[0]) {
            free(mirror);
            gf_log(GF_LOG_ERROR, "no commit identity to extract");
            return -1;
        }
        if (gf_git_cache_extract_tree(mirror, r->src.commit, dest_dir) != 0) {
            free(mirror);
            return -1;
        }
        free(mirror);
    }
    return 0;
}
