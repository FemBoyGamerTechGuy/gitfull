/* ops.c — command implementations (install/remove/update/...). */
#include "cli.h"
#include "commands.h"

#include "build.h"
#include "common.h"
#include "depgraph.h"
#include "forge.h"
#include "http.h"
#include "json.h"
#include "package.h"
#include "pkgid.h"
#include "recipe.h"
#include "sandbox.h"
#include "sha256.h"
#include "source.h"
#include "store.h"
#include "tar.h"
#include "xact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* activation: store payload -> live filesystem, transactionally       */
/* ------------------------------------------------------------------ */

/* Remove empty parent directories of a removed file, walking up toward
 * the system root. Shared directories are never empty and stop the walk. */
static void prune_empty_parents(const char *root, const char *file_target)
{
    char *cur = gf_path_dirname(file_target);
    for (;;) {
        if (!cur || !*cur)
            break;
        if (root && *root && strcmp(cur, root) == 0)
            break;
        if (gf_fs_rmdir_if_empty(cur) != 0)
            break; /* not empty (or not ours to remove) */
        char *parent = gf_path_dirname(cur);
        free(cur);
        cur = parent;
    }
    free(cur);
}

static int activate_package(gf_cmdctx *ctx, const gf_build_result *br,
                            const char *confirm_prompt,
                            const char *history_detail)
{
    gf_config *cfg = ctx->cfg;
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    int fail_rc = GF_EXIT_XACT; /* conflict paths override this below */

    char *name = gf_strdup(br->meta->name);
    char *version = gf_strdup(br->meta->version);
    char *build_id = gf_strdup(br->meta->build_id);

    /* 1. read the package + manifest from the store */
    char *pkg_path = gf_store_pkg_path(cfg->state_dir, name, version, build_id);
    gf_pkgmeta *meta = NULL;
    gf_manifest_entry *entries = NULL;
    size_t ne = 0;
    if (gf_package_open(pkg_path, &meta, &entries, &ne, NULL) != 0) {
        gf_log(GF_LOG_ERROR, "cannot open package from store: %s", pkg_path);
        goto fail;
    }

    /* 2. pre-flight: every target path must be free or owned by us.
     * pkg_files stores root-RELATIVE paths; lookups must match.
     * Directories are shared infrastructure (usr/local/bin belongs to
     * many packages) and never conflict — only files and symlinks do. */
    for (size_t i = 0; i < ne; i++) {
        if (entries[i].type == 'd')
            continue;
        char *target = gf_path_join(cfg->root, entries[i].path);
        struct stat st;
        if (lstat(target, &st) == 0) {
            char *owner = gf_db_file_owner(db, entries[i].path);
            if (!owner) {
                gf_log(GF_LOG_ERROR,
                       "file conflict: %s exists but is not owned by any "
                       "package", target);
                free(target);
                fail_rc = GF_EXIT_CONFLICT;
                goto fail_pkg;
            }
            bool mine = strcmp(owner, name) == 0;
            free(owner);
            if (!mine) {
                gf_log(GF_LOG_ERROR,
                       "file conflict: %s already owned by another package "
                       "(owner lookup: %s)", target, entries[i].path);
                free(target);
                fail_rc = GF_EXIT_CONFLICT;
                goto fail_pkg;
            }
        }
        free(target);
    }
    if (ctx->cfg->interactive && !ctx->yes) {
        if (!gf_cli_confirm(ctx, confirm_prompt)) {
            gf_log(GF_LOG_INFO, "aborted");
            goto fail_pkg;
        }
    }

    /* 3. transaction */
    gf_xact *x = gf_xact_begin(cfg, db, "install", name);
    if (!x)
        goto fail_pkg;
    if (gf_db_begin(db) != 0) {
        gf_xact_free(x);
        goto fail_pkg;
    }

    char *files_dir = gf_store_files_path(cfg->state_dir, name, version, build_id);
    int applied = 0;
    for (size_t i = 0; i < ne; i++) {
        char *target = gf_path_join(cfg->root, entries[i].path);
        if (entries[i].type == 'd') {
            if (gf_xact_mkdir(x, target) != 0) {
                free(target);
                goto fail_apply;
            }
        } else if (entries[i].type == 'l') {
            /* symlink: create directly (journal handles undo) */
            char *parent = gf_path_dirname(target);
            gf_fs_mkdir_p(parent);
            free(parent);
            if (gf_fs_symlink(entries[i].target ? entries[i].target : "", target) != 0) {
                /* maybe replace */
                unlink(target);
                if (gf_fs_symlink(entries[i].target ? entries[i].target : "", target) != 0) {
                    gf_log(GF_LOG_ERROR, "cannot create symlink %s", target);
                    free(target);
                    goto fail_apply;
                }
            }
            /* journal the symlink creation for undo */
            {
                char *src = gf_path_join(files_dir, entries[i].path);
                if (gf_fs_exists(src))
                    gf_xact_remove_file(x, target);
                free(src);
            }
        } else if (entries[i].type == 'f') {
            char *src = gf_path_join(files_dir, entries[i].path);
            if (gf_xact_install_file(x, src, target, &entries[i]) != 0) {
                free(src);
                free(target);
                goto fail_apply;
            }
            free(src);
        }
        free(target);
        applied++;
    }

    /* 4. DB records within the same transaction. Directories are shared
     * infrastructure and intentionally NOT recorded (many packages own
     * usr/local/bin); only files and symlinks get ownership rows. */
    gf_db_remove_files(db, name);
    for (size_t i = 0; i < ne; i++) {
        if (entries[i].type == 'd')
            continue;
        char *target = gf_path_join(cfg->root, entries[i].path);
        /* record path RELATIVE to root for lookups */
        const char *rel = target;
        if (cfg->root && *cfg->root) {
            rel = target + strlen(cfg->root);
            if (*rel == '/')
                rel++;
        } else {
            rel = entries[i].path;
        }
        if (gf_db_add_file(db, name, rel, entries[i].type, entries[i].mode,
                           entries[i].size, entries[i].sha256,
                           entries[i].target) != 0) {
            free(target);
            goto fail_apply;
        }
        free(target);
    }
    /* keep the old version's rollback slot (payload already in store) */
    gf_db_deactivate_slots(db, name);
    {
        gf_db_pkg *old = gf_db_get_package(db, name);
        if (old) {
            gf_db_add_slot(db, name, old->meta.version, old->meta.build_id, false);
            gf_db_pkg_free(old);
        }
    }
    /* DB records use the BUILD's metadata (build-id, pkg sha, deps);
     * the re-opened package only provides the manifest entries. */
    if (gf_db_put_package(db, br->meta, br->build_dir) != 0)
        goto fail_apply;
    gf_db_add_slot(db, name, version, build_id, true);
    gf_db_add_history(db, name, version, "installed",
                      history_detail ? history_detail : "");
    if (gf_db_commit(db) != 0)
        goto fail_apply;
    gf_xact_commit(x);
    gf_xact_free(x);

    gf_pkgmeta_free_heap(meta);
    gf_manifest_free(entries, ne);
    free(files_dir);
    free(pkg_path);
    free(name);
    free(version);
    free(build_id);
    return 0;

fail_apply:
    gf_db_rollback(db);
    gf_xact_rollback(x);
    gf_xact_free(x);
    gf_log(GF_LOG_ERROR, "installation rolled back (previous state restored)");
fail_pkg:
    gf_pkgmeta_free_heap(meta);
    gf_manifest_free(entries, ne);
fail:
    free(pkg_path);
    free(name);
    free(version);
    free(build_id);
    return fail_rc;
}

/* ------------------------------------------------------------------ */
/* install                                                            */
/* ------------------------------------------------------------------ */

typedef struct install_opts {
    bool edge;
    bool prerelease;
    bool skip_tests;
    bool force;
    bool dry_run;
    bool resolve_deps;   /* false for recursive dep installs (graph already
                          * handled by the root invocation) */
} install_opts;

/* ------------------------------------------------------------------ */
/* dependency resolution: recipe-driven graph, cycles, conflicts,       */
/* arch/ABI checks, topological install order                           */
/* ------------------------------------------------------------------ */

#define GF_DEP_MAX_DEPTH 64

typedef struct dep_resolver {
    gf_cmdctx *ctx;
    const install_opts *io;
    gf_dep_graph *graph;
    /* per-node recipe arch/abi (parallel to graph->nodes, set once) */
    char **arch;
    char **abi;
    size_t nslots;      /* == graph->n at all times */
    int depth;
} dep_resolver;

static void dep_resolver_free(dep_resolver *dr)
{
    if (!dr)
        return;
    for (size_t i = 0; dr->arch && i < dr->nslots; i++)
        free(dr->arch[i]);
    for (size_t i = 0; dr->abi && i < dr->nslots; i++)
        free(dr->abi[i]);
    free(dr->arch);
    free(dr->abi);
    gf_depgraph_free(dr->graph);
    free(dr);
}

/* slot index for a node (appends arch/abi slots as the graph grows) */
static size_t dep_slot(dep_resolver *dr, gf_dep_node *n)
{
    for (size_t i = 0; i < dr->graph->n; i++) {
        if (dr->graph->nodes[i] == n)
            return i;
    }
    return 0; /* unreachable when the node came from this graph */
}

/* Fetch (and cache) the recipe of a resolved package by peeking its source.
 * Returns the recipe or NULL (not fatal: recipe-less packages are simply
 * autodetected builds with no dependencies of their own). */
static gf_recipe *peek_recipe(gf_cmdctx *ctx, gf_resolved *res,
                              const char *name)
{
    char *peekdir = gf_path_join_multi(ctx->cfg->state_dir, "builds", name,
                                       res->is_edge ? "edge" : res->version,
                                       ".peek", NULL);
    gf_recipe *r = NULL;
    if (gf_fs_exists(peekdir) ||
        gf_source_acquire(ctx->cfg, res, peekdir) == 0) {
        r = gf_recipe_load(peekdir);
    }
    free(peekdir);
    return r;
}

/* Add a dependency edge parent -> child, creating the child node when new.
 * spec is the recipe spec string; version constraints in it (either a bare
 * constraint like "^1.2" or the @version part of a package ref) merge into
 * the node's accumulated constraint set. Returns the child node. */
static gf_dep_node *dep_add(dep_resolver *dr, gf_dep_node *parent,
                            const char *name, const char *spec, bool build_dep)
{
    gf_pkgref dref;
    char *extracted = NULL;
    if (spec && gf_pkgref_parse(spec, dr->ctx->cfg->default_repo, &dref) == 0) {
        if (dref.version && *dref.version)
            extracted = gf_strdup(dref.version);
        gf_pkgref_free(&dref);
    }
    gf_dep_node *child = gf_depgraph_require(dr->graph, name, spec, build_dep);
    /* merge a ref-style @version into the constraint set */
    if (extracted) {
        (void)gf_depgraph_require(dr->graph, name, extracted, build_dep);
        free(extracted);
    }
    gf_depgraph_add_edge(dr->graph, parent, child);
    return child;
}

/* Expand one node: read its recipe, register arch/abi, recurse into its
 * dependencies. Returns 0/-1 (hard errors: unresolvable required deps,
 * arch conflicts, depth exceeded). */
static int dep_expand(dep_resolver *dr, gf_dep_node *node,
                      gf_resolved *node_res)
{
    gf_cmdctx *ctx = dr->ctx;
    gf_db *db = gf_cli_db(ctx);

    if (++dr->depth > GF_DEP_MAX_DEPTH) {
        gf_log(GF_LOG_ERROR,
               "dependency nesting deeper than %d levels; refusing to "
               "continue (cycle guard)", GF_DEP_MAX_DEPTH);
        return -1;
    }

    gf_recipe *recipe = node_res ? peek_recipe(ctx, node_res, node->name)
                                 : NULL;
    if (!recipe) {
        dr->depth--;
        return 0; /* no recipe: leaf package, autodetected build */
    }

    /* grow the aux arrays to match the graph */
    if (dr->graph->n > dr->nslots) {
        size_t want = dr->graph->n;
        dr->arch = gf_realloc(dr->arch, want * sizeof(char *));
        dr->abi = gf_realloc(dr->abi, want * sizeof(char *));
        for (size_t i = dr->nslots; i < want; i++) {
            dr->arch[i] = NULL;
            dr->abi[i] = NULL;
        }
        dr->nslots = want;
    }
    size_t slot = dep_slot(dr, node);
    if (recipe->arch && !dr->arch[slot])
        dr->arch[slot] = gf_strdup(recipe->arch);
    if (recipe->abi && !dr->abi[slot])
        dr->abi[slot] = gf_strdup(recipe->abi);

    /* arch constraint: hard error (the binary cannot run/build here) */
    if (recipe->arch && strcmp(recipe->arch, ctx->cfg->arch) != 0) {
        gf_log(GF_LOG_ERROR,
               "architecture conflict: %s requires arch '%s' but this host "
               "is '%s'", node->name, recipe->arch, ctx->cfg->arch);
        gf_recipe_free(recipe);
        dr->depth--;
        return -1;
    }

    /* expansion order: runtime deps, build deps, optional, test */
    typedef struct { char **names; char **specs; size_t n; bool build;
                     bool opt; } dl;
    dl dls[4];
    dls[0] = (dl){ recipe->dep_names, recipe->dep_specs, recipe->ndeps,
                  false, false };
    dls[1] = (dl){ recipe->bdep_names, recipe->bdep_specs, recipe->nbdeps,
                  true, false };
    dls[2] = (dl){ recipe->odep_names, recipe->odep_specs, recipe->nodeps,
                  false, true };
    dls[3] = (dl){ recipe->tdep_names, recipe->tdep_specs, recipe->ntdeps,
                  true, false };
    bool run_tests = dr->io->skip_tests ? false
        : (recipe->run_tests_set ? recipe->run_tests
                                 : ctx->cfg->run_tests);

    int rc = 0;
    for (int li = 0; li < 4 && rc == 0; li++) {
        if (li == 3 && !run_tests)
            break; /* test dependencies only when tests run */
        for (size_t i = 0; i < dls[li].n && rc == 0; i++) {
            const char *dname = dls[li].names[i];
            const char *dspec = dls[li].specs[i];
            if (!dname || !*dname)
                continue;
            gf_dep_node *child = dep_add(dr, node, dname, dspec, dls[li].build);
            /* already-expanded nodes are skipped (diamonds converge) */
            if (child->state == GF_DEP_DONE ||
                (li == 2 && gf_depgraph_find(dr->graph, dname) != child)) {
                continue;
            }
            /* resolve the child's source (optional deps may fail) */
            gf_pkgref cref;
            char crefspec[1024];
            snprintf(crefspec, sizeof(crefspec), "%s",
                     dspec && strchr(dspec, ':') ? dspec
                     : (dspec && strchr(dspec, '/') ? dspec : ""));
            gf_resolved *cres = NULL;
            if (*crefspec &&
                gf_pkgref_parse(crefspec, ctx->cfg->default_repo, &cref) == 0) {
                gf_resolve_opts cropts = { 0 };
                if (cref.version && *cref.version)
                    cropts.exact_tag = cref.version;
                cres = gf_resolve(ctx->cfg, &cref, &cropts);
            } else {
                memset(&cref, 0, sizeof(cref));
                /* bare name: only satisfiable when already installed */
                if (db && gf_db_get_package(db, dname)) {
                    gf_db_pkg *p = gf_db_get_package(db, dname);
                    gf_log(GF_LOG_INFO,
                           "dependency %s satisfied by installed %s",
                           dname, p->meta.version);
                    gf_db_pkg_free(p);
                    child->state = GF_DEP_DONE;
                    continue;
                }
                if (dls[li].opt) {
                    gf_log(GF_LOG_INFO,
                           "optional dependency '%s' has no source spec "
                           "and is not installed; skipping", dname);
                    child->state = GF_DEP_DONE;
                    continue;
                }
                gf_log(GF_LOG_ERROR,
                       "dependency '%s' of %s has no qualified source spec "
                       "(use \"name = \"github:owner/repo@ver\"\" in "
                       "gitfull.toml) and is not installed", dname,
                       node->name);
                rc = -1;
                break;
            }
            if (!cres) {
                if (dls[li].opt) {
                    gf_log(GF_LOG_INFO,
                           "optional dependency '%s' not resolvable; "
                           "skipping", dname);
                    child->state = GF_DEP_DONE;
                    continue;
                }
                gf_log(GF_LOG_ERROR, "dependency '%s' of %s not resolvable",
                       dname, node->name);
                rc = -1;
                break;
            }
            /* already installed at a satisfying version? mark done */
            if (db && !dls[li].build) {
                gf_db_pkg *p = gf_db_get_package(db, dname);
                if (p) {
                    char *instver = gf_strdup(p->meta.version);
                    gf_db_pkg_free(p);
                    gf_version v;
                    bool sat = false;
                    if (gf_version_parse(instver, &v)) {
                        sat = gf_vcset_empty(&child->want) ||
                              gf_vcset_match(&child->want, &v);
                        gf_version_free(&v);
                    }
                    if (sat) {
                        child->version = instver;
                        gf_pkgref_free(&cref);
                        gf_resolved_free(cres);
                        child->state = GF_DEP_DONE;
                        continue;
                    }
                    /* installed but wrong version: resolver will pick the
                     * constrained one below (upgrade of the dependency) */
                    free(instver);
                }
            }
            rc = dep_expand(dr, child, cres);
            gf_pkgref_free(&cref);
            gf_resolved_free(cres);
        }
    }

    node->state = GF_DEP_DONE;
    gf_recipe_free(recipe);
    dr->depth--;
    return rc;
}

/* ------------------------------------------------------------------ */
/* dependency plan: copied out of the graph so it outlives the resolver */
/* ------------------------------------------------------------------ */

typedef struct dep_plan_entry {
    char *name;
    char *spec;          /* source spec (ref form) or NULL (constraint-only) */
    gf_vcset want;       /* deep-copied merged constraints */
    bool build_dep;
    bool satisfied;      /* already installed at a satisfying version */
    char *version;       /* installed version when satisfied */
} dep_plan_entry;

static void dep_plan_free(dep_plan_entry *plan, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(plan[i].name);
        free(plan[i].spec);
        free(plan[i].version);
        gf_vcset_free(&plan[i].want);
    }
    free(plan);
}

static gf_vcset dep_vcset_copy(const gf_vcset *s)
{
    gf_vcset out = { NULL, 0 };
    if (!s || s->n == 0)
        return out;
    out.cons = gf_malloc(s->n * sizeof(gf_vc));
    out.n = s->n;
    for (size_t i = 0; i < s->n; i++) {
        out.cons[i] = s->cons[i];
        out.cons[i].ver = gf_version_copy(&s->cons[i].ver);
        if (s->cons[i].ver.raw)
            out.cons[i].ver.raw = gf_strdup(s->cons[i].ver.raw);
    }
    return out;
}

/* Is this dep spec a package reference (as opposed to a bare constraint)? */
static bool dep_spec_is_ref(const char *spec)
{
    if (!spec || !*spec)
        return false;
    if (strstr(spec, "://") || strchr(spec, '/'))
        return true;
    if (gf_str_starts_with(spec, "local:"))
        return true;
    /* forge-qualified: "github:owner/repo" (has a colon but no slash-free) */
    return strchr(spec, ':') != NULL;
}

/* Resolve the dependency graph for 'ref'. On success returns 0 and fills
 * *out_plan (owned) + *out_n: entries in topological order, dependencies
 * before dependents, with the ROOT as the last entry. On failure returns
 * an exit code (cycle/conflict messages already logged). */
static int resolve_dependency_graph(gf_cmdctx *ctx, const gf_pkgref *ref,
                                    const install_opts *io,
                                    gf_resolved *res,
                                    dep_plan_entry **out_plan, size_t *out_n)
{
    char *name = gf_pkgref_name(ref);
    if (!name)
        return GF_EXIT_USAGE;

    dep_resolver *dr = gf_calloc(1, sizeof(dep_resolver));
    dr->ctx = ctx;
    dr->io = io;
    dr->graph = gf_depgraph_new();

    gf_dep_node *root = gf_depgraph_require(dr->graph, name, NULL, false);
    root->state = GF_DEP_DONE;
    int rc = dep_expand(dr, root, res);
    (void)io;
    free(name);
    if (rc != 0) {
        dep_resolver_free(dr);
        return GF_EXIT_CONFLICT;
    }

    /* cycles: report the full chain */
    char **chain = NULL;
    size_t chain_len = 0;
    if (gf_depgraph_check_cycles(dr->graph, &chain, &chain_len) != 0) {
        gf_strbuf sb;
        gf_strbuf_init(&sb);
        for (size_t i = 0; i < chain_len; i++) {
            gf_strbuf_append(&sb, chain[i]);
            if (i + 1 < chain_len)
                gf_strbuf_append(&sb, " -> ");
        }
        gf_log(GF_LOG_ERROR, "dependency cycle detected: %s",
               gf_strbuf_str(&sb));
        gf_strbuf_free(&sb);
        gf_strv_free(chain, chain_len);
        dep_resolver_free(dr);
        return GF_EXIT_CONFLICT;
    }

    /* constraint conflicts (exact-vs-exact, resolved-vs-constraints) */
    if (gf_depgraph_check_conflicts(dr->graph) != 0) {
        dep_resolver_free(dr);
        return GF_EXIT_CONFLICT;
    }

    /* ABI mismatch between the root and any dependency: warning (C ABIs
     * interoperate case-by-case), recorded in the log for diagnosis. */
    {
        char *rname = gf_pkgref_name(ref);
        gf_recipe *root_recipe = peek_recipe(ctx, res, rname);
        if (root_recipe && root_recipe->abi) {
            for (size_t i = 0; i < dr->graph->n; i++) {
                if (dr->abi && dr->abi[i] &&
                    strcmp(dr->abi[i], root_recipe->abi) != 0) {
                    gf_log(GF_LOG_WARN,
                           "abi mismatch: root uses abi '%s' but dependency "
                           "'%s' builds with abi '%s'",
                           root_recipe->abi, dr->graph->nodes[i]->name,
                           dr->abi[i]);
                }
            }
        }
        gf_recipe_free(root_recipe);
        free(rname);
    }

    gf_dep_node **order = NULL;
    size_t n = 0;
    if (gf_depgraph_topo(dr->graph, &order, &n) != 0) {
        dep_resolver_free(dr);
        return GF_EXIT_CONFLICT;
    }

    /* copy the plan out of the graph (nodes die with the resolver) */
    dep_plan_entry *plan = NULL;
    if (n > 0)
        plan = gf_calloc(n, sizeof(dep_plan_entry));
    for (size_t i = 0; i < n; i++) {
        gf_dep_node *nd = order[i];
        plan[i].name = gf_strdup(nd->name);
        plan[i].spec = nd->spec && dep_spec_is_ref(nd->spec)
                           ? gf_strdup(nd->spec)
                           : NULL;
        plan[i].want = dep_vcset_copy(&nd->want);
        plan[i].build_dep = nd->build_dep;
        if (nd->version) {
            plan[i].satisfied = true;
            plan[i].version = gf_strdup(nd->version);
        }
    }
    free(order);
    dep_resolver_free(dr);
    *out_plan = plan;
    *out_n = n;
    return 0;
}

static int install_one(gf_cmdctx *ctx, const gf_pkgref *ref,
                       const install_opts *io, bool install_files,
                       gf_build_dep *out_dep);

/* Execute a dependency plan: install runtime deps, build build-deps, and
 * collect their build-ids so the root build can stage their payloads.
 * Returns 0 with out_bdeps/out_nbdeps owned, or an exit code. */
static int execute_dep_plan(gf_cmdctx *ctx, dep_plan_entry *plan, size_t nplan,
                            const install_opts *io,
                            gf_build_dep **out_bdeps, size_t *out_nbdeps)
{
    gf_config *cfg = ctx->cfg;
    gf_db *db = gf_cli_db(ctx);
    gf_build_dep *bdeps = NULL;
    size_t nbdeps = 0;

    for (size_t i = 0; i + 1 < nplan; i++) { /* last entry is the root */
        dep_plan_entry *e = &plan[i];
        gf_build_dep bd = { 0 };

        if (e->satisfied && !e->build_dep && db) {
            gf_db_pkg *p = gf_db_get_package(db, e->name);
            if (p) {
                bd.name = gf_strdup(e->name);
                bd.version = gf_strdup(p->meta.version);
                bd.build_id = gf_strdup(p->meta.build_id);
                gf_db_pkg_free(p);
                gf_log(GF_LOG_INFO, "dependency %s %s satisfied (installed)",
                       e->name, bd.version);
            }
        }
        if (!bd.name) {
            gf_pkgref dref;
            if (!e->spec ||
                gf_pkgref_parse(e->spec, cfg->default_repo, &dref) != 0) {
                gf_log(GF_LOG_ERROR,
                       "dependency '%s' cannot be resolved (spec '%s')",
                       e->name, e->spec ? e->spec : "(none)");
                dep_plan_free(plan, nplan);
                for (size_t k = 0; k < nbdeps; k++) {
                    free(bdeps[k].name); free(bdeps[k].version);
                    free(bdeps[k].build_id);
                }
                free(bdeps);
                return GF_EXIT_CONFLICT;
            }
            /* a single exact constraint pins the version */
            if (e->want.n == 1 && e->want.cons[0].op == GF_VC_EXACT &&
                !dref.version)
                dref.version = gf_strdup(e->want.cons[0].ver.raw);
            install_opts dio = *io;
            dio.resolve_deps = false; /* the graph already ordered deps */
            gf_log(GF_LOG_INFO, "%s dependency %s (%s)",
                   e->build_dep ? "building" : "installing", e->name,
                   e->spec);
            int drc = install_one(ctx, &dref, &dio, !e->build_dep, &bd);
            gf_pkgref_free(&dref);
            if (drc != 0) {
                dep_plan_free(plan, nplan);
                for (size_t k = 0; k < nbdeps; k++) {
                    free(bdeps[k].name); free(bdeps[k].version);
                    free(bdeps[k].build_id);
                }
                free(bdeps);
                return drc;
            }
            /* verify the constraint set against the resolved version */
            gf_version v;
            if (bd.version && gf_version_parse(bd.version, &v)) {
                if (!gf_vcset_empty(&e->want) &&
                    !gf_vcset_match(&e->want, &v)) {
                    char *specs = gf_vcset_str(&e->want);
                    gf_log(GF_LOG_ERROR,
                           "dependency %s resolved to %s which does not "
                           "satisfy %s", e->name, bd.version,
                           specs ? specs : "?");
                    free(specs);
                    gf_version_free(&v);
                    free(bd.name); free(bd.version); free(bd.build_id);
                    dep_plan_free(plan, nplan);
                    for (size_t k = 0; k < nbdeps; k++) {
                        free(bdeps[k].name); free(bdeps[k].version);
                        free(bdeps[k].build_id);
                    }
                    free(bdeps);
                    return GF_EXIT_CONFLICT;
                }
                gf_version_free(&v);
            }
        }
        bdeps = gf_realloc(bdeps, (nbdeps + 1) * sizeof(gf_build_dep));
        bdeps[nbdeps++] = bd;
    }
    *out_bdeps = bdeps;
    *out_nbdeps = nbdeps;
    return 0;
}

static void build_deps_free(gf_build_dep *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(d[i].name);
        free(d[i].version);
        free(d[i].build_id);
    }
    free(d);
}

/* full install pipeline for one package */
static int install_one(gf_cmdctx *ctx, const gf_pkgref *ref,
                       const install_opts *io, bool install_files,
                       gf_build_dep *out_dep)
{
    gf_config *cfg = ctx->cfg;
    gf_db *db = gf_cli_db(ctx);

    gf_resolve_opts ropts = { 0 };
    ropts.edge = io->edge;
    ropts.prerelease = io->prerelease;
    if (ref->version && *ref->version)
        ropts.exact_tag = ref->version;

    gf_resolved *res = gf_resolve(cfg, ref, &ropts);
    if (!res)
        return GF_EXIT_NOTFOUND;

    char *name = gf_pkgref_name(ref);

    /* already installed at this version? */
    if (db && install_files) {
        gf_db_pkg *cur = gf_db_get_package(db, name);
        if (cur && strcmp(cur->meta.version, res->version) == 0 &&
            !res->is_edge && !io->force) {
            gf_log(GF_LOG_INFO, "%s %s is already installed", name,
                   res->version);
            if (out_dep) {
                out_dep->name = gf_strdup(name);
                out_dep->version = gf_strdup(cur->meta.version);
                out_dep->build_id = gf_strdup(cur->meta.build_id);
            }
            gf_db_pkg_free(cur);
            gf_resolved_free(res);
            free(name);
            return 0;
        }
        gf_db_pkg_free(cur);
    }

    gf_log(GF_LOG_INFO, "resolving %s: %s (%s, %s)", name, res->version,
           gf_src_status_name(res->src.status),
           res->src.commit ? res->src.commit : "no commit");

    /* ---- dependency resolution (root invocation only) ---- */
    gf_build_dep *bdeps = NULL;
    size_t nbdeps = 0;
    if (io->resolve_deps) {
        dep_plan_entry *plan = NULL;
        size_t nplan = 0;
        int grc = resolve_dependency_graph(ctx, ref, io, res, &plan, &nplan);
        if (grc != 0) {
            gf_resolved_free(res);
            free(name);
            return grc;
        }
        if (nplan > 1)
            gf_log(GF_LOG_INFO, "dependency plan: %zu package(s) in order",
                   nplan - 1);
        if (io->dry_run) {
            printf("would install %s %s\n", name, res->version);
            for (size_t i = 0; i + 1 < nplan; i++) {
                char *specs = gf_vcset_str(&plan[i].want);
                printf("  %-28s %-10s %s%s\n", plan[i].name,
                       plan[i].build_dep ? "build" : "runtime",
                       specs ? specs : "", plan[i].satisfied ? " (installed)" : "");
                free(specs);
            }
            dep_plan_free(plan, nplan);
            gf_resolved_free(res);
            free(name);
            return 0;
        }
        int erc = execute_dep_plan(ctx, plan, nplan, io, &bdeps, &nbdeps);
        dep_plan_free(plan, nplan); /* entries consumed into bdeps */
        if (erc != 0) {
            gf_resolved_free(res);
            free(name);
            return erc;
        }
    }

    /* build */
    gf_build_opts bopts = { 0 };
    bopts.skip_tests = io->skip_tests;
    bopts.force = io->force;
    bopts.deps = bdeps;
    bopts.ndeps = nbdeps;
    gf_build_result br;
    memset(&br, 0, sizeof(br));
    int brc = gf_build_package(cfg, db, res, &bopts, &br);
    build_deps_free(bdeps, nbdeps);
    if (brc != 0) {
        gf_resolved_free(res);
        free(name);
        return brc;
    }

    if (out_dep) {
        out_dep->name = gf_strdup(name);
        out_dep->version = gf_strdup(br.meta->version);
        out_dep->build_id = gf_strdup(br.build_id);
    }

    int rc = 0;
    if (install_files) {
        char note[256];
        snprintf(note, sizeof(note), "install %s %s to %s?", name,
                 br.meta->version, cfg->prefix);
        char detail[256];
        const char *srcspec = res->ref.raw ? res->ref.raw : name;
        snprintf(detail, sizeof(detail), "source=%s build=%.12s",
                 srcspec, br.build_id ? br.build_id : "?");
        rc = activate_package(ctx, &br, note, detail);
    }
    gf_build_result_free(&br);
    gf_resolved_free(res);
    free(name);
    return rc;
}

int cmd_install(gf_cmdctx *ctx, int argc, char **argv)
{
    install_opts io = { 0 };
    io.resolve_deps = true;
    const char *spec = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--edge") == 0)
            io.edge = true;
        else if (strcmp(argv[i], "--prerelease") == 0)
            io.prerelease = true;
        else if (strcmp(argv[i], "--skip-tests") == 0)
            io.skip_tests = true;
        else if (strcmp(argv[i], "--force") == 0)
            io.force = true;
        else if (strcmp(argv[i], "--dry-run") == 0)
            io.dry_run = true;
        else if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
            ctx->yes = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("%s\n", gf_cli_find("install")->help);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            gf_log(GF_LOG_ERROR, "unknown option: %s", argv[i]);
            return GF_EXIT_USAGE;
        } else if (!spec)
            spec = argv[i];
        else {
            gf_log(GF_LOG_ERROR, "unexpected argument: %s", argv[i]);
            return GF_EXIT_USAGE;
        }
    }
    if (!spec) {
        gf_log(GF_LOG_ERROR, "install needs a package identifier");
        return GF_EXIT_USAGE;
    }
    if (!strchr(spec, '/') && !gf_str_starts_with(spec, "local:") &&
        !strstr(spec, "://")) {
        gf_log(GF_LOG_ERROR,
               "'%s' is not a qualified package reference.\n"
               "  install accepts: owner/repository, github:owner/repo,\n"
               "  codeberg:owner/repo, gitlab:owner/repo, git+https://...,\n"
               "  https://host/owner/repo, local:/path — use\n"
               "  `gitfull search %s` to find the owner/repository form",
               spec, spec);
        return GF_EXIT_USAGE;
    }

    gf_pkgref ref;
    if (gf_pkgref_parse(spec, ctx->cfg->default_repo, &ref) != 0)
        return GF_EXIT_USAGE;
    return install_one(ctx, &ref, &io, true, NULL);
}

/* ------------------------------------------------------------------ */
/* remove                                                             */
/* ------------------------------------------------------------------ */

int cmd_remove(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *name = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
            ctx->yes = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("remove")->help);
            return 0;
        } else if (!name)
            name = argv[i];
    }
    if (!name) {
        gf_log(GF_LOG_ERROR, "remove needs a package name");
        return GF_EXIT_USAGE;
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    gf_db_pkg *p = gf_db_get_package(db, name);
    if (!p) {
        gf_log(GF_LOG_ERROR, "package not installed: %s", name);
        return GF_EXIT_NOTFOUND;
    }

    /* refuse when other packages depend on it */
    size_t nrdep = 0;
    char **rdeps = gf_db_reverse_deps(db, name, &nrdep);
    if (nrdep > 0) {
        gf_log(GF_LOG_ERROR, "cannot remove %s: required by:", name);
        for (size_t i = 0; i < nrdep; i++)
            gf_log(GF_LOG_ERROR, "  %s", rdeps[i]);
        gf_strv_free(rdeps, nrdep);
        gf_db_pkg_free(p);
        return GF_EXIT_CONFLICT;
    }
    gf_strv_free(rdeps, nrdep);

    char note[256];
    snprintf(note, sizeof(note), "remove %s %s?", name, p->meta.version);
    if (!gf_cli_confirm(ctx, note)) {
        gf_db_pkg_free(p);
        return 0;
    }

    gf_manifest_entry *entries = NULL;
    size_t ne = 0;
    gf_db_package_files(db, name, &entries, &ne);

    gf_xact *x = gf_xact_begin(ctx->cfg, db, "remove", name);
    if (!x) {
        gf_db_pkg_free(p);
        return GF_EXIT_XACT;
    }
    if (gf_db_begin(db) != 0) {
        gf_xact_free(x);
        gf_db_pkg_free(p);
        return GF_EXIT_XACT;
    }
    int rc = 0;
    for (size_t i = 0; i < ne && rc == 0; i++) {
        char *target = gf_path_join(ctx->cfg->root, entries[i].path);
        if (entries[i].type == 'd')
            gf_fs_rmdir_if_empty(target);
        else
            rc = gf_xact_remove_file(x, target);
        if (rc == 0)
            prune_empty_parents(ctx->cfg->root, target);
        free(target);
    }
    if (rc == 0) {
        gf_db_remove_files(db, name);
        gf_db_remove_package(db, name);
        gf_db_add_history(db, name, p->meta.version, "removed", "");
        gf_db_deactivate_slots(db, name);
        if (gf_db_commit(db) != 0)
            rc = -1;
    }
    if (rc != 0) {
        gf_db_rollback(db);
        gf_xact_rollback(x);
        gf_log(GF_LOG_ERROR, "removal rolled back");
        rc = GF_EXIT_XACT;
    } else {
        gf_xact_commit(x);
        gf_log(GF_LOG_INFO, "removed %s %s (payload retained for rollback)",
               name, p->meta.version);
    }
    gf_xact_free(x);
    gf_manifest_free(entries, ne);
    gf_db_pkg_free(p);
    return rc;
}

/* ------------------------------------------------------------------ */
/* update / upgrade                                                   */
/* ------------------------------------------------------------------ */

/* Is candidate 'newer' than installed? Semver comparison when both parse
 * (so downgrades are not offered); string inequality otherwise. */
static bool version_newer(const char *cand, const char *installed)
{
    gf_version a, b;
    if (gf_version_parse(cand, &a) && gf_version_parse(installed, &b)) {
        bool newer = gf_version_cmp(&a, &b) > 0;
        gf_version_free(&a);
        gf_version_free(&b);
        return newer;
    }
    return cand && installed && strcmp(cand, installed) != 0;
}

int cmd_update(gf_cmdctx *ctx, int argc, char **argv)
{
    bool apply = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--apply") == 0)
            apply = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("update")->help);
            return 0;
        }
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    size_t n = 0;
    char **pkgs = gf_db_list_packages(db, &n);
    int upgradable = 0;
    for (size_t i = 0; i < n; i++) {
        gf_db_pkg *p = gf_db_get_package(db, pkgs[i]);
        if (!p)
            continue;
        /* holds outrank every source type: a held package is never
         * reported as updatable, local or remote */
        if (gf_db_is_held(db, pkgs[i])) {
            printf("%-24s %-14s HOLD\n", pkgs[i], p->meta.version);
            gf_db_pkg_free(p);
            continue;
        }
        if (p->meta.forge && strcmp(p->meta.forge, "local") == 0) {
            /* local sources: re-resolve (local git repos expose tags, so a
             * newer stable tag is a genuine update candidate) */
            bool resolved_local = false;
            if (p->meta.repo_url && *p->meta.repo_url) {
                gf_pkgref lref;
                char lspec[4200];
                snprintf(lspec, sizeof(lspec), "local:%s", p->meta.repo_url);
                if (gf_pkgref_parse(lspec, NULL, &lref) == 0) {
                    gf_resolve_opts lropts = { 0 };
                    gf_resolved *lres = gf_resolve(ctx->cfg, &lref, &lropts);
                    gf_pkgref_free(&lref);
                    if (lres) {
                        resolved_local = true;
                        if (!lres->is_edge && lres->version &&
                            version_newer(lres->version, p->meta.version)) {
                            printf("%-24s %-14s update available (%s)\n",
                                   pkgs[i], p->meta.version, lres->version);
                            upgradable++;
                        } else {
                            printf("%-24s %-14s up to date (local)\n", pkgs[i],
                                   p->meta.version);
                        }
                        gf_resolved_free(lres);
                    }
                }
            }
            if (!resolved_local)
                printf("%-24s %-14s local (not checkable: no source path)\n",
                       pkgs[i], p->meta.version);
            gf_db_pkg_free(p);
            continue;
        }
        /* resolve latest stable */
        gf_pkgref ref;
        char spec[512];
        snprintf(spec, sizeof(spec), "%s", pkgs[i]);
        gf_pkgref_parse(spec, NULL, &ref);
        /* find the source repo from the DB record */
        if (p->meta.forge && p->meta.repo_url) {
            free(ref.forge);
            ref.forge = gf_strdup(p->meta.forge);
            /* owner/repo from repo_url is unknown; use forge:owner/repo if
             * we stored owner... we stored repo_url only. Use the URL form. */
            free(ref.url);
            ref.url = gf_strdup(p->meta.repo_url);
        }
        gf_resolve_opts ropts = { 0 };
        gf_resolved *res = gf_resolve(ctx->cfg, &ref, &ropts);
        if (res && !res->is_edge && res->version &&
            version_newer(res->version, p->meta.version)) {
            printf("%-24s %-14s update available (%s)\n", pkgs[i],
                   p->meta.version, res->version);
            upgradable++;
            gf_resolved_free(res);
        } else {
            printf("%-24s %-14s up to date\n", pkgs[i], p->meta.version);
            if (res)
                gf_resolved_free(res);
        }
        gf_pkgref_free(&ref);
        gf_db_pkg_free(p);
        if (apply && upgradable > 0) {
            /* handled below */
        }
    }
    gf_strv_free(pkgs, n);
    if (upgradable == 0) {
        printf("all packages up to date\n");
        return 0;
    }
    if (!apply) {
        printf("\nrun `gitfull upgrade` to apply %d update(s)\n", upgradable);
        return 0;
    }
    /* apply: re-run as upgrade */
    char up[] = "upgrade";
    char *fake_argv[] = { up, NULL };
    return cmd_upgrade(ctx, 1, fake_argv);
}

int cmd_upgrade(gf_cmdctx *ctx, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0)
            ctx->yes = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("upgrade")->help);
            return 0;
        }
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    size_t n = 0;
    char **pkgs = gf_db_list_packages(db, &n);
    int upgraded = 0, rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        if (gf_db_is_held(db, pkgs[i])) {
            printf("%-24s HOLD (skipped)\n", pkgs[i]);
            continue;
        }
        gf_db_pkg *p = gf_db_get_package(db, pkgs[i]);
        if (!p)
            continue;
        if (p->meta.forge && strcmp(p->meta.forge, "local") == 0) {
            /* local sources: upgrade when a newer stable tag exists */
            if (p->meta.repo_url && *p->meta.repo_url) {
                gf_pkgref lref;
                char lspec[4200];
                snprintf(lspec, sizeof(lspec), "local:%s", p->meta.repo_url);
                if (gf_pkgref_parse(lspec, NULL, &lref) == 0) {
                    gf_resolve_opts lropts = { 0 };
                    gf_resolved *lres = gf_resolve(ctx->cfg, &lref, &lropts);
                    if (lres && !lres->is_edge && lres->version &&
                        version_newer(lres->version, p->meta.version)) {
                        gf_log(GF_LOG_INFO, "upgrading %s: %s -> %s", pkgs[i],
                               p->meta.version, lres->version);
                        install_opts io = { 0 };
                        io.force = true;
                        int lrc = install_one(ctx, &lref, &io, true, NULL);
                        if (lrc == 0)
                            upgraded++;
                        else if (lrc != GF_EXIT_NOTFOUND)
                            rc = lrc;
                    } else {
                        printf("%-24s up to date (local)\n", pkgs[i]);
                    }
                    gf_resolved_free(lres);
                    gf_pkgref_free(&lref);
                }
            }
            gf_db_pkg_free(p);
            continue;
        }
        gf_pkgref ref;
        memset(&ref, 0, sizeof(ref));
        if (p->meta.forge && p->meta.repo_url) {
            ref.forge = gf_strdup(p->meta.forge);
            ref.url = gf_strdup(p->meta.repo_url);
            /* derive owner/repo from the URL */
            char *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
            if (gf_url_split(p->meta.repo_url, &scheme, &host, &port, &path) == 0) {
                if (path && path[0] == '/' && strchr(path + 1, '/')) {
                    char *slash = strchr(path + 1, '/');
                    *slash = '\0';
                    ref.owner = gf_strdup(path + 1);
                    char *rest = slash + 1;
                    char *dotgit = strstr(rest, ".git");
                    if (dotgit)
                        *dotgit = '\0';
                    ref.repo = gf_strdup(rest);
                }
                free(scheme);
                free(host);
                free(port);
                free(path);
            }
        }
        if (!ref.repo) {
            gf_pkgref_free(&ref);
            gf_db_pkg_free(p);
            continue;
        }
        gf_resolve_opts ropts = { 0 };
        gf_resolved *res = gf_resolve(ctx->cfg, &ref, &ropts);
        if (res && !res->is_edge && res->version &&
            version_newer(res->version, p->meta.version)) {
            gf_log(GF_LOG_INFO, "upgrading %s: %s -> %s", pkgs[i],
                   p->meta.version, res->version);
            install_opts io = { 0 };
            io.force = true;
            rc = install_one(ctx, &ref, &io, true, NULL);
            if (rc == 0)
                upgraded++;
        }
        if (res)
            gf_resolved_free(res);
        gf_pkgref_free(&ref);
        gf_db_pkg_free(p);
    }
    gf_strv_free(pkgs, n);
    if (upgraded > 0)
        gf_log(GF_LOG_INFO, "upgraded %d package(s)", upgraded);
    return rc;
}

/* ------------------------------------------------------------------ */
/* rollback                                                           */
/* ------------------------------------------------------------------ */

int cmd_rollback(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *name = NULL;
    const char *to_version = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--to") == 0 && i + 1 < argc)
            to_version = argv[++i];
        else if (strcmp(argv[i], "--yes") == 0)
            ctx->yes = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("rollback")->help);
            return 0;
        } else if (!name)
            name = argv[i];
    }
    if (!name) {
        gf_log(GF_LOG_ERROR, "rollback needs a package name");
        return GF_EXIT_USAGE;
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    gf_db_pkg *cur = gf_db_get_package(db, name);
    if (!cur) {
        gf_log(GF_LOG_ERROR, "package not installed: %s", name);
        return GF_EXIT_NOTFOUND;
    }

    gf_db_slot *slots = NULL;
    size_t nslots = 0;
    gf_db_slots(db, name, &slots, &nslots);

    if (nslots == 0) {
        gf_log(GF_LOG_ERROR, "no rollback versions retained for %s", name);
        gf_db_pkg_free(cur);
        return GF_EXIT_NOTFOUND;
    }

    size_t pick = 0;
    if (to_version) {
        bool found = false;
        for (size_t i = 0; i < nslots; i++) {
            if (strcmp(slots[i].version, to_version) == 0) {
                pick = i;
                found = true;
                break;
            }
        }
        if (!found) {
            gf_log(GF_LOG_ERROR, "version %s not available for rollback",
                   to_version);
            gf_db_pkg_free(cur);
            gf_db_slots_free(slots, nslots);
            return GF_EXIT_NOTFOUND;
        }
    } else {
        /* most recent deactivated slot */
        for (size_t i = 0; i < nslots; i++) {
            if (!slots[i].active) {
                pick = i;
                break;
            }
        }
        if (slots[pick].active) {
            gf_log(GF_LOG_ERROR, "no previous version to roll back to");
            gf_db_pkg_free(cur);
            gf_db_slots_free(slots, nslots);
            return GF_EXIT_NOTFOUND;
        }
    }

    gf_db_slot *target = &slots[pick];
    printf("rollback %s: %s -> %s\n", name, cur->meta.version, target->version);

    /* payload must still exist in the store */
    char *pkg_path = gf_store_pkg_path(ctx->cfg->state_dir, name,
                                       target->version, target->build_id);
    if (!gf_fs_is_file(pkg_path)) {
        gf_log(GF_LOG_ERROR, "rollback payload missing in store");
        free(pkg_path);
        gf_db_pkg_free(cur);
        gf_db_slots_free(slots, nslots);
        return GF_EXIT_NOTFOUND;
    }

    /* build a gf_build_result-like activation from the store */
    gf_pkgmeta *meta = NULL;
    gf_manifest_entry *entries = NULL;
    size_t ne = 0;
    if (gf_package_open(pkg_path, &meta, &entries, &ne, NULL) != 0) {
        free(pkg_path);
        gf_db_pkg_free(cur);
        gf_db_slots_free(slots, nslots);
        return GF_EXIT_ERROR;
    }
    /* note: meta and pkg_path ownership transfers to the activation below */

    gf_build_result br;
    memset(&br, 0, sizeof(br));
    br.build_id = gf_strdup(target->build_id);
    br.pkg_path = NULL; /* owned locally: pkg_path */
    br.meta = NULL;     /* owned locally: meta */
    br.build_dir = NULL;

    /* remove current files (they stay in store as a rollback slot),
     * then activate the target version */
    gf_xact *x = gf_xact_begin(ctx->cfg, db, "rollback", name);
    if (!x) {
        gf_pkgmeta_free_heap(meta);
        gf_manifest_free(entries, ne);
        gf_db_pkg_free(cur);
        gf_db_slots_free(slots, nslots);
        return GF_EXIT_XACT;
    }
    if (gf_db_begin(db) != 0) {
        gf_xact_free(x);
        gf_pkgmeta_free_heap(meta);
        gf_manifest_free(entries, ne);
        gf_db_pkg_free(cur);
        gf_db_slots_free(slots, nslots);
        return GF_EXIT_XACT;
    }

    gf_manifest_entry *cur_entries = NULL;
    size_t cne = 0;
    gf_db_package_files(db, name, &cur_entries, &cne);
    int rc = 0;
    for (size_t i = 0; i < cne && rc == 0; i++) {
        char *target_path = gf_path_join(ctx->cfg->root, cur_entries[i].path);
        if (cur_entries[i].type == 'd')
            gf_fs_rmdir_if_empty(target_path);
        else
            rc = gf_xact_remove_file(x, target_path);
        if (rc == 0)
            prune_empty_parents(ctx->cfg->root, target_path);
        free(target_path);
    }
    gf_manifest_free(cur_entries, cne);

    char *files_dir = gf_store_files_path(ctx->cfg->state_dir, name,
                                          target->version, target->build_id);
    for (size_t i = 0; i < ne && rc == 0; i++) {
        char *target_path = gf_path_join(ctx->cfg->root, entries[i].path);
        if (entries[i].type == 'd') {
            gf_xact_mkdir(x, target_path);
        } else if (entries[i].type == 'l') {
            char *parent = gf_path_dirname(target_path);
            gf_fs_mkdir_p(parent);
            free(parent);
            gf_fs_symlink(entries[i].target ? entries[i].target : "", target_path);
        } else {
            char *src = gf_path_join(files_dir, entries[i].path);
            rc = gf_xact_install_file(x, src, target_path, &entries[i]);
            free(src);
        }
        free(target_path);
    }
    free(files_dir);

    if (rc == 0) {
        gf_db_remove_files(db, name);
        for (size_t i = 0; i < ne; i++) {
            if (entries[i].type == 'd')
                continue; /* directories are not owned */
            char *target_path = gf_path_join(ctx->cfg->root, entries[i].path);
            const char *rel = *ctx->cfg->root
                ? target_path + strlen(ctx->cfg->root)
                : target_path;
            while (*rel == '/')
                rel++;
            gf_db_add_file(db, name, rel, entries[i].type, entries[i].mode,
                           entries[i].size, entries[i].sha256,
                           entries[i].target);
            free(target_path);
        }
        /* current version becomes a rollback slot; target becomes active */
        gf_db_add_slot(db, name, cur->meta.version, cur->meta.build_id, false);
        gf_db_drop_slot(db, name, target->version, target->build_id);
        char *bd = gf_strdup(cur->build_dir ? cur->build_dir : "");
        gf_db_put_package(db, meta, bd);
        free(bd);
        gf_db_add_slot(db, name, target->version, target->build_id, true);
        gf_db_add_history(db, name, target->version, "rolled_back",
                          cur->meta.version);
        if (gf_db_commit(db) != 0)
            rc = -1;
    }
    if (rc != 0) {
        gf_db_rollback(db);
        gf_xact_rollback(x);
        gf_log(GF_LOG_ERROR, "rollback failed; previous state restored");
        rc = GF_EXIT_XACT;
    } else {
        gf_xact_commit(x);
        gf_log(GF_LOG_INFO, "rolled back %s to %s", name, target->version);
    }
    gf_xact_free(x);
    gf_pkgmeta_free_heap(meta);
    gf_manifest_free(entries, ne);
    gf_db_pkg_free(cur);
    gf_db_slots_free(slots, nslots);
    return rc;
}

/* ------------------------------------------------------------------ */
/* holds                                                              */
/* ------------------------------------------------------------------ */

int cmd_unhold(gf_cmdctx *ctx, int argc, char **argv)
{
    if (argc == 0) {
        gf_log(GF_LOG_ERROR, "unhold needs a package name");
        return GF_EXIT_USAGE;
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    gf_db_unhold(db, argv[0]);
    gf_db_add_history(db, argv[0], NULL, "unhold", "");
    printf("%s unheld\n", argv[0]);
    return 0;
}

int cmd_hold(gf_cmdctx *ctx, int argc, char **argv)
{
    if (argc == 0) {
        gf_log(GF_LOG_ERROR, "usage: gitfull hold <package> | hold list");
        return GF_EXIT_USAGE;
    }
    if (strcmp(argv[0], "list") == 0) {
        gf_db *db = gf_cli_db(ctx);
        if (!db)
            return GF_EXIT_ERROR;
        size_t n = 0;
        char **holds = gf_db_holds(db, &n);
        if (n == 0) {
            printf("no held packages\n");
            return 0;
        }
        for (size_t i = 0; i < n; i++)
            printf("%s\n", holds[i]);
        gf_strv_free(holds, n);
        return 0;
    }
    const char *sub = argv[0];
    const char *name = argc > 1 ? argv[1] : NULL;
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    if (strcmp(sub, "unhold") == 0) {
        if (!name) {
            gf_log(GF_LOG_ERROR, "unhold needs a package name");
            return GF_EXIT_USAGE;
        }
        gf_db_unhold(db, name);
        gf_db_add_history(db, name, NULL, "unhold", "");
        printf("%s unheld\n", name);
        return 0;
    }
    if (strcmp(sub, "list") != 0) {
        /* `gitfull hold <pkg>` (and `gitfull hold hold <pkg>` legacy form) */
        name = strcmp(sub, "hold") == 0 ? name : sub;
    }
    if (!name) {
        gf_log(GF_LOG_ERROR, "hold needs a package name");
        return GF_EXIT_USAGE;
    }
    gf_db_hold(db, name, "manual");
    gf_db_add_history(db, name, NULL, "hold", "");
    printf("%s held (skipped by update/upgrade)\n", name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* list / info / search / history / logs                              */
/* ------------------------------------------------------------------ */

int cmd_list(gf_cmdctx *ctx, int argc, char **argv)
{
    (void)argv;
    if (argc > 0 && strcmp(argv[0], "--help") == 0) {
        printf("%s\n", gf_cli_find("list")->help);
        return 0;
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    size_t n = 0;
    char **pkgs = gf_db_list_packages(db, &n);
    if (n == 0) {
        printf("no packages installed\n");
        return 0;
    }
    printf("%-24s %-16s %-10s %-10s %s\n", "PACKAGE", "VERSION", "ARCH",
           "STATUS", "SOURCE");
    for (size_t i = 0; i < n; i++) {
        gf_db_pkg *p = gf_db_get_package(db, pkgs[i]);
        if (!p)
            continue;
        printf("%-24s %-16s %-10s %-10s %s\n", pkgs[i], p->meta.version,
               p->meta.arch, gf_db_is_held(db, pkgs[i]) ? "HOLD" : "ok",
               p->meta.forge ? p->meta.forge : "-");
        gf_db_pkg_free(p);
    }
    gf_strv_free(pkgs, n);
    return 0;
}

int cmd_info(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *spec = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("info")->help);
            return 0;
        }
        if (!spec)
            spec = argv[i];
    }
    if (!spec) {
        gf_log(GF_LOG_ERROR, "info needs a package");
        return GF_EXIT_USAGE;
    }
    gf_db *db = gf_cli_db(ctx);

    /* installed? */
    if (db) {
        gf_db_pkg *p = gf_db_get_package(db, spec);
        if (p) {
            printf("name:        %s\n", p->meta.name);
            printf("version:     %s\n", p->meta.version);
            printf("architecture:%s\n", p->meta.arch);
            printf("build-id:    %s\n", p->meta.build_id);
            printf("source:      %s\n", p->meta.repo_url);
            printf("commit:      %s\n", p->meta.commit);
            printf("source sha:  %s\n", p->meta.source_sha);
            printf("verified:    %s\n", p->meta.source_status);
            printf("license:     %s\n", p->meta.license);
            printf("description: %s\n", p->meta.description);
            printf("installed:   %s\n", p->installed_at);
            if (p->updated_at && *p->updated_at)
                printf("updated:     %s\n", p->updated_at);
            printf("hold:        %s\n", gf_db_is_held(db, spec) ? "yes" : "no");
            if (p->meta.ndeps > 0) {
                printf("depends on:\n");
                for (size_t i = 0; i < p->meta.ndeps; i++)
                    printf("  %s %s (%s)\n", p->meta.dep_names[i],
                           p->meta.dep_specs[i], p->meta.dep_kinds[i]);
            }
            size_t nfiles = 0;
            gf_manifest_entry *files = NULL;
            gf_db_package_files(db, spec, &files, &nfiles);
            printf("files:       %zu\n", nfiles);
            gf_manifest_free(files, nfiles);
            gf_db_pkg_free(p);
            return 0;
        }
    }

    /* remote */
    gf_pkgref ref;
    if (gf_pkgref_parse(spec, ctx->cfg->default_repo, &ref) != 0)
        return GF_EXIT_USAGE;
    gf_resolve_opts ropts = { 0 };
    gf_resolved *res = gf_resolve(ctx->cfg, &ref, &ropts);
    if (!res)
        return GF_EXIT_NOTFOUND;
    printf("name:        %s\n", ref.repo);
    printf("version:     %s\n", res->version);
    printf("source:      %s\n", res->src.repo_url);
    printf("commit:      %s\n", res->src.commit ? res->src.commit : "-");
    printf("verified:    %s\n", gf_src_status_name(res->src.status));
    printf("description: %s\n",
           res->src.description ? res->src.description : "-");
    printf("license:     %s\n", res->src.license ? res->src.license : "-");
    printf("status:      not installed\n");
    gf_resolved_free(res);
    gf_pkgref_free(&ref);
    return 0;
}

int cmd_search(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *query = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("search")->help);
            return 0;
        }
        if (!query)
            query = argv[i];
    }
    if (!query) {
        gf_log(GF_LOG_ERROR, "search needs a query");
        return GF_EXIT_USAGE;
    }
    bool any = false;
    for (size_t r = 0; ; r++) {
        const gf_repo *repo = gf_config_repo_enabled(ctx->cfg, r);
        if (!repo)
            break;
        gf_forge *f = gf_forge_open(repo, ctx->cfg);
        if (!f || !f->ops->search)
            continue;
        gf_json *results = NULL;
        if (f->ops->search(f, query, &results) == 0 && results) {
            size_t n = gf_json_len(results);
            for (size_t i = 0; i < n; i++) {
                const gf_json *item = gf_json_at(results, i);
                if (!item)
                    continue;
                const char *full = gf_json_str(gf_json_get(item, "full_name"));
                const char *desc = gf_json_str(gf_json_get(item, "description"));
                const char *url = gf_json_str(gf_json_get(item, "html_url"));
                if (!full) {
                    const char *path = gf_json_str(gf_json_get(item, "path"));
                    if (path) /* gitlab search shape */
                        full = path;
                }
                if (!full)
                    continue;
                printf("%-40s %s\n", full, desc ? desc : "");
                (void)url;
                any = true;
            }
            gf_json_free(results);
        }
        gf_forge_close(f);
    }
    if (!any)
        printf("no results for '%s'\n", query);
    return 0;
}

int cmd_history(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *pkg = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("history")->help);
            return 0;
        }
        if (!pkg)
            pkg = argv[i];
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    gf_db_hist *h = NULL;
    size_t n = 0;
    gf_db_history(db, pkg, &h, &n);
    if (n == 0) {
        printf("no history\n");
        return 0;
    }
    printf("%-20s %-12s %-14s %s\n", "AT", "EVENT", "VERSION", "DETAIL");
    for (size_t i = 0; i < n; i++) {
        printf("%-20s %-12s %-14s %s\n", h[i].at, h[i].event, h[i].version,
               h[i].detail);
    }
    gf_db_hist_free(h, n);
    return 0;
}

int cmd_logs(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *pkg = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("logs")->help);
            return 0;
        }
        if (!pkg)
            pkg = argv[i];
    }
    if (!pkg) {
        gf_log(GF_LOG_ERROR, "logs needs a package name");
        return GF_EXIT_USAGE;
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    gf_db_pkg *p = gf_db_get_package(db, pkg);
    if (!p || !p->build_dir || !*p->build_dir) {
        gf_log(GF_LOG_ERROR, "no build logs for %s", pkg);
        gf_db_pkg_free(p);
        return GF_EXIT_NOTFOUND;
    }
    char *logp = gf_path_join(p->build_dir, "logs/build.log");
    char *text = gf_fs_read_file_limit(logp, 16 << 20, NULL);
    if (!text) {
        /* fallback: cat the latest builds dir */
        char *bdir = gf_path_join_multi(ctx->cfg->state_dir, "builds", pkg,
                                        NULL);
        printf("build dir: %s\n", bdir);
        size_t n = 0;
        char **versions = NULL;
        if (gf_fs_list_dir(bdir, &versions, &n) == 0) {
            for (size_t i = 0; i < n; i++)
                printf("  %s\n", versions[i]);
            gf_strv_free(versions, n);
        }
        free(bdir);
    } else {
        printf("%s", text);
        free(text);
    }
    free(logp);
    gf_db_pkg_free(p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* verify / build                                                     */
/* ------------------------------------------------------------------ */

int cmd_verify(gf_cmdctx *ctx, int argc, char **argv)
{
    const char *pkg = NULL;
    bool all = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--all") == 0)
            all = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("verify")->help);
            return 0;
        } else if (!pkg)
            pkg = argv[i];
    }
    gf_db *db = gf_cli_db(ctx);
    if (!db)
        return GF_EXIT_ERROR;
    if (!pkg && !all) {
        gf_log(GF_LOG_ERROR, "verify needs a package or --all");
        return GF_EXIT_USAGE;
    }
    char **pkgs = NULL;
    size_t n = 0;
    if (all)
        pkgs = gf_db_list_packages(db, &n);
    else {
        pkgs = gf_malloc(2 * sizeof(char *));
        pkgs[0] = gf_strdup(pkg);
        pkgs[1] = NULL;
        n = 1;
    }
    int rc = 0;
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        gf_db_pkg *p = gf_db_get_package(db, pkgs[i]);
        if (!p)
            continue;
        gf_manifest_entry *files = NULL;
        size_t nf = 0;
        gf_db_package_files(db, pkgs[i], &files, &nf);
        int missing = 0, drifted = 0;
        for (size_t k = 0; k < nf; k++) {
            if (files[k].type != 'f')
                continue;
            char *target = gf_path_join(ctx->cfg->root, files[k].path);
            if (!gf_fs_exists(target)) {
                missing++;
                printf("%s: MISSING %s\n", pkgs[i], files[k].path);
            } else {
                char *h = gf_sha256_file_hex(target);
                if (h && files[k].sha256 && strcmp(h, files[k].sha256) != 0) {
                    drifted++;
                    printf("%s: CHANGED %s\n", pkgs[i], files[k].path);
                }
                free(h);
            }
            free(target);
        }
        if (missing == 0 && drifted == 0)
            printf("%-24s ok (%zu files)\n", pkgs[i], nf);
        else
            bad++;
        gf_manifest_free(files, nf);
        gf_db_pkg_free(p);
    }
    gf_strv_free(pkgs, n);
    if (bad > 0) {
        gf_log(GF_LOG_ERROR, "%d package(s) failed verification", bad);
        return GF_EXIT_ERROR;
    }
    return rc;
}

int cmd_build(gf_cmdctx *ctx, int argc, char **argv)
{
    install_opts io = { 0 };
    io.resolve_deps = true;
    const char *spec = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--edge") == 0)
            io.edge = true;
        else if (strcmp(argv[i], "--prerelease") == 0)
            io.prerelease = true;
        else if (strcmp(argv[i], "--skip-tests") == 0)
            io.skip_tests = true;
        else if (strcmp(argv[i], "--force") == 0)
            io.force = true;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("%s\n", gf_cli_find("build")->help);
            return 0;
        } else if (!spec)
            spec = argv[i];
    }
    if (!spec) {
        gf_log(GF_LOG_ERROR, "build needs a package identifier");
        return GF_EXIT_USAGE;
    }
    gf_pkgref ref;
    if (gf_pkgref_parse(spec, ctx->cfg->default_repo, &ref) != 0)
        return GF_EXIT_USAGE;
    gf_build_dep dep = { 0 };
    int rc = install_one(ctx, &ref, &io, false, &dep);
    if (rc == 0)
        printf("built without installing: %s %s (%s)\n", dep.name, dep.version,
               dep.build_id);
    free(dep.name);
    free(dep.version);
    free(dep.build_id);
    gf_pkgref_free(&ref);
    return rc;
}

/* ------------------------------------------------------------------ */
/* doctor / clean / cache / repo / config                             */
/* ------------------------------------------------------------------ */

#include "doctor.h"
int cmd_doctor(gf_cmdctx *ctx, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return gf_doctor_run(ctx, 0, NULL);
}

#include "clean.h"
int cmd_clean(gf_cmdctx *ctx, int argc, char **argv)
{
    return gf_clean_run(ctx, argc, argv);
}

int cmd_cache(gf_cmdctx *ctx, int argc, char **argv)
{
    (void)argv;
    if (argc > 0 && strcmp(argv[0], "clean") == 0) {
        char *c1 = gf_path_join(ctx->cfg->state_dir, "cache/tarballs");
        char *c2 = gf_path_join(ctx->cfg->state_dir, "cache/git");
        uint64_t before = 0;
        gf_fs_du(c1, &before);
        uint64_t g = 0;
        gf_fs_du(c2, &g);
        before += g;
        gf_fs_rm_rf(c1);
        gf_fs_rm_rf(c2);
        printf("cleared caches (%llu bytes)\n", (unsigned long long)before);
        free(c1);
        free(c2);
        return 0;
    }
    char *c1 = gf_path_join(ctx->cfg->state_dir, "cache/tarballs");
    char *c2 = gf_path_join(ctx->cfg->state_dir, "cache/git");
    uint64_t t = 0, g = 0;
    gf_fs_du(c1, &t);
    gf_fs_du(c2, &g);
    uint64_t s = gf_store_usage(ctx->cfg->state_dir);
    printf("tarball cache: %llu bytes\ngit mirrors:   %llu bytes\nstore:         %llu bytes\n",
           (unsigned long long)t, (unsigned long long)g,
           (unsigned long long)s);
    free(c1);
    free(c2);
    return 0;
}

int cmd_repo(gf_cmdctx *ctx, int argc, char **argv)
{
    if (argc == 0) {
        gf_log(GF_LOG_ERROR, "repo needs a subcommand (list|add|remove|enable|disable)");
        return GF_EXIT_USAGE;
    }
    const char *sub = argv[0];
    if (strcmp(sub, "list") == 0) {
        printf("%-14s %-10s %-8s %s\n", "NAME", "KIND", "ENABLED", "API");
        for (size_t i = 0; i < ctx->cfg->nrepos; i++) {
            const gf_repo *r = &ctx->cfg->repos[i];
            printf("%-14s %-10s %-8s %s\n", r->name, r->kind,
                   r->enabled ? "yes" : "no", r->api_url);
        }
        return 0;
    }
    const char *conf_path = ctx->cfg->path ? ctx->cfg->path : ctx->cfg->etc_conf;
    if (strcmp(sub, "add") == 0) {
        if (argc < 4) {
            gf_log(GF_LOG_ERROR, "repo add <name> <kind> <api-url> [--enable]");
            return GF_EXIT_USAGE;
        }
        gf_repo r = { 0 };
        r.name = gf_strdup(argv[1]);
        r.kind = gf_strdup(argv[2]);
        r.api_url = gf_strdup(argv[3]);
        r.web_url = gf_strdup(argv[3]);
        r.enabled = false;
        for (int i = 4; i < argc; i++)
            if (strcmp(argv[i], "--enable") == 0)
                r.enabled = true;
        if (!gf_str_starts_with(r.api_url, "https://")) {
            gf_log(GF_LOG_ERROR, "api-url must be https://");
            return GF_EXIT_USAGE;
        }
        if (!gf_fs_is_file(conf_path)) {
            /* create the config with the repo appended */
            gf_fs_mkdir_p(gf_path_dirname(conf_path));
            gf_fs_write_file_atomic(conf_path, gf_config_default_text(),
                                    strlen(gf_config_default_text()));
        }
        if (gf_config_file_repo_add(conf_path, &r) != 0) {
            return GF_EXIT_ERROR;
        }
        printf("repository '%s' added to %s\n", r.name, conf_path);
        return 0;
    }
    if (strcmp(sub, "remove") == 0 && argc >= 2) {
        if (!ctx->cfg->path) {
            gf_log(GF_LOG_ERROR, "no config file in use (%s)", conf_path);
            return GF_EXIT_ERROR;
        }
        if (gf_config_file_repo_remove(conf_path, argv[1]) != 0)
            return GF_EXIT_ERROR;
        printf("repository '%s' removed\n", argv[1]);
        return 0;
    }
    if ((strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) &&
        argc >= 2) {
        if (!ctx->cfg->path) {
            gf_log(GF_LOG_ERROR, "no config file in use (%s)", conf_path);
            return GF_EXIT_ERROR;
        }
        /* implement as remove + re-add with the flag toggled */
        const gf_repo *cur = gf_config_repo(ctx->cfg, argv[1]);
        if (!cur) {
            gf_log(GF_LOG_ERROR, "unknown repository: %s", argv[1]);
            return GF_EXIT_NOTFOUND;
        }
        gf_repo copy = { 0 };
        copy.name = gf_strdup(cur->name);
        copy.kind = gf_strdup(cur->kind);
        copy.api_url = gf_strdup(cur->api_url);
        copy.web_url = gf_strdup(cur->web_url);
        copy.token_file = cur->token_file ? gf_strdup(cur->token_file) : NULL;
        copy.token_env = cur->token_env ? gf_strdup(cur->token_env) : NULL;
        copy.enabled = strcmp(sub, "enable") == 0;
        if (gf_config_file_repo_remove(conf_path, argv[1]) != 0) {
            return GF_EXIT_ERROR;
        }
        if (gf_config_file_repo_add(conf_path, &copy) != 0) {
            return GF_EXIT_ERROR;
        }
        printf("repository '%s' %sd\n", argv[1], sub);
        return 0;
    }
    gf_log(GF_LOG_ERROR, "unknown repo subcommand: %s", sub);
    return GF_EXIT_USAGE;
}

int cmd_config(gf_cmdctx *ctx, int argc, char **argv)
{
    if (argc == 0) {
        gf_log(GF_LOG_ERROR, "config needs a subcommand (show|validate|path)");
        return GF_EXIT_USAGE;
    }
    if (strcmp(argv[0], "path") == 0) {
        printf("%s\n", ctx->cfg->path ? ctx->cfg->path : ctx->cfg->etc_conf);
        printf("(file exists: %s)\n",
               gf_fs_is_file(ctx->cfg->path ? ctx->cfg->path : ctx->cfg->etc_conf)
                   ? "yes"
                   : "no, using built-in defaults");
        return 0;
    }
    if (strcmp(argv[0], "show") == 0) {
        printf("config file:  %s\n",
               ctx->cfg->path ? ctx->cfg->path : "(built-in defaults)");
        printf("state dir:    %s\n", ctx->cfg->state_dir);
        printf("prefix:       %s\n", ctx->cfg->prefix);
        printf("architecture: %s\n", ctx->cfg->arch);
        printf("default repo: %s\n", ctx->cfg->default_repo);
        printf("toolchain:    %s\n", ctx->cfg->toolchain_mode);
        printf("sandbox:      %s (network: %s, seccomp: %s)\n",
               ctx->cfg->sandbox_level, ctx->cfg->sandbox_network,
               ctx->cfg->sandbox_seccomp ? "on" : "off");
        printf("res limits:   memory %s, cpu %s\n",
               ctx->cfg->memory_limit_mb > 0
                   ? "(set)" : "none",
               ctx->cfg->cpu_quota_percent > 0 ? "(set)" : "none");
        printf("rollback:     keep %d version(s)\n", ctx->cfg->rollback_hold);
        printf("jobs:         %d\n", ctx->cfg->jobs);
        printf("run_tests:    %s\n", ctx->cfg->run_tests ? "true" : "false");
        printf("repositories:\n");
        for (size_t i = 0; i < ctx->cfg->nrepos; i++)
            printf("  %-12s kind=%-8s enabled=%-4s %s\n",
                   ctx->cfg->repos[i].name, ctx->cfg->repos[i].kind,
                   ctx->cfg->repos[i].enabled ? "true" : "false",
                   ctx->cfg->repos[i].api_url);
        return 0;
    }
    if (strcmp(argv[0], "validate") == 0) {
        if (gf_config_validate(ctx->cfg) != 0) {
            printf("config INVALID\n");
            return GF_EXIT_ERROR;
        }
        printf("config OK (%s)\n",
               ctx->cfg->path ? ctx->cfg->path : "built-in defaults");
        return 0;
    }
    gf_log(GF_LOG_ERROR, "unknown config subcommand: %s", argv[0]);
    return GF_EXIT_USAGE;
}
