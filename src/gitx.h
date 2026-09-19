/* gitx.h — Git operations via controlled child processes.
 *
 * Credential safety: tokens are never placed in URLs, argv, or .git/config.
 * An ephemeral GIT_ASKPASS helper (0600, deleted after use) provides
 * credentials only to the git child process.
 */
#ifndef GF_GITX_H
#define GF_GITX_H

#include <stdbool.h>
#include <stddef.h>

/* Is git usable? version copied when buffers given. */
int gf_git_check(char *version, size_t versionlen);

/* ls-remote: resolve a ref (tag, branch, "HEAD") on a remote to a sha.
 * sha_out must hold 41+ chars. Returns 0/-1. */
int gf_git_ls_remote(const char *url, const char *ref, char *sha_out,
                     const char *token);

/* Discover the remote HEAD symref (default branch). Returns malloc'd branch
 * name and 40-char sha. */
int gf_git_ls_remote_head(const char *url, char **branch_out, char *sha_out,
                          const char *token);

/* Bare mirror cache path for a URL under the gitfull state dir. */
char *gf_git_cache_path(const char *state_dir, const char *url);

/* Ensure the mirror exists and the given refs are fetched (refs fetched:
 * tags + heads). Token may be NULL. Returns 0/-1. */
int gf_git_cache_fetch(const char *mirror_path, const char *url,
                       const char *token);

/* Extract the exact tree of a commit sha from a mirror into dest_dir using
 * plumbing (read-tree + checkout-index). dest must be an existing empty dir.
 * Deterministic: no index/worktree state leaks. */
int gf_git_cache_extract_tree(const char *mirror_path, const char *sha,
                              const char *dest_dir);

/* Parse a revision inside a repo dir to a full sha (41+ buffer). */
int gf_git_rev_parse(const char *repo_dir, const char *rev, char *sha_out);

/* List tag names in a local repo (refs/tags, short names). Returns a
 * malloc'd array of malloc'd strings in *out, count in *count (0 with
 * *out=NULL when no tags). Caller frees each entry and the array. 0/-1. */
int gf_git_list_tags(const char *repo_dir, char ***out, size_t *count);

/* Verify a tag's signature in repo_dir.
 * outcomes: 0 signed+valid, 1 unsigned, 2 bad signature (error!), -1 cannot
 * check (gpg missing). */
int gf_git_verify_tag(const char *repo_dir, const char *tag);

/* Full clone + detached checkout of exact sha into dest (no mirror). */
int gf_git_clone_exact(const char *url, const char *sha, const char *dest,
                       const char *token);

#endif /* GF_GITX_H */
