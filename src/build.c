/* build.c — sandboxed, logged, reproducible build pipeline. */
#include "build.h"

#include "common.h"
#include "detect.h"
#include "forge.h"
#include "gitx.h"
#include "json.h"
#include "recipe.h"
#include "sandbox.h"
#include "sha256.h"
#include "store.h"
#include "tar.h"
#include "toolchain.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Frees the OWNED FIELDS of a build result. The container itself is not
 * freed (callers pass stack-allocated gf_build_result structs). */
void gf_build_result_free(gf_build_result *r)
{
    if (!r)
        return;
    free(r->build_id);
    free(r->pkg_path);
    gf_pkgmeta_free_heap(r->meta);
    free(r->build_dir);
    r->build_id = NULL;
    r->pkg_path = NULL;
    r->meta = NULL;
    r->build_dir = NULL;
}

/* ---------------------------------------------------- build identity */

static int64_t commit_epoch(const gf_config *cfg, const gf_resolved *res)
{
    if (!res->src.repo_url || !res->src.commit || !res->src.commit[0])
        return 0;
    if (res->ref.local)
        return 0;
    char *mirror = gf_git_cache_path(cfg->state_dir, res->src.repo_url);
    char fmt[128];
    snprintf(fmt, sizeof(fmt), "%s^{commit}", res->src.commit);
    const char *argv[] = { "git", "--git-dir", mirror, "show", "-s",
                           "--format=%ct", fmt, NULL };
    gf_strbuf out;
    gf_strbuf_init(&out);
    gf_exec_opts eo = gf_exec_opts_default();
    eo.out = &out;
    gf_exec_result rst;
    memset(&rst, 0, sizeof(rst));
    int64_t epoch = 0;
    if (gf_exec_capture(argv, &eo, &rst) == 0 && rst.status == 0) {
        char *t = gf_strbuf_steal(&out);
        gf_str_trim(t);
        epoch = strtoll(t, NULL, 10);
        free(t);
    } else {
        gf_strbuf_free(&out);
    }
    free(mirror);
    return epoch;
}

/* ---------------------------------------------------- env construction */

/* env array builder: list of "K=V" strings, NULL terminated; owned. */
static char **env_new(void)
{
    char **e = gf_malloc(64 * sizeof(char *));
    e[0] = NULL;
    return e;
}
static void env_push(char ***env, size_t *n, const char *kv)
{
    (*env)[(*n)++] = gf_strdup(kv);
    (*env)[*n] = NULL;
}
static void env_free(char **env, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(env[i]);
    free(env);
}

/* ---------------------------------------------------- build plans */

typedef struct plan_step {
    char **argv;
    const char *cwd;   /* "src" or "build" */
    char *desc;
    bool is_test;
} plan_step;

typedef struct build_plan {
    plan_step *steps;
    size_t n;
    char *system;      /* build system name */
} build_plan;

static void plan_free(build_plan *p)
{
    for (size_t i = 0; i < p->n; i++) {
        for (size_t k = 0; p->steps[i].argv[k]; k++)
            free(p->steps[i].argv[k]);
        free(p->steps[i].argv);
        free(p->steps[i].desc);
    }
    free(p->steps);
    free(p->system);
    memset(p, 0, sizeof(*p));
}

static void plan_add(build_plan *p, char *const *argv, const char *cwd,
                     const char *desc, bool is_test)
{
    p->steps = gf_realloc(p->steps, (p->n + 1) * sizeof(plan_step));
    plan_step *s = &p->steps[p->n++];
    size_t na = gf_argv_len((const char *const *)argv);
    s->argv = gf_malloc((na + 1) * sizeof(char *));
    for (size_t i = 0; i < na; i++)
        s->argv[i] = gf_strdup(argv[i]);
    s->argv[na] = NULL;
    s->cwd = cwd;
    s->desc = gf_strdup(desc);
    s->is_test = is_test;
}

static void plan_addv(build_plan *p, const char *const *argv, const char *cwd,
                     const char *desc, bool is_test)
{
    p->steps = gf_realloc(p->steps, (p->n + 1) * sizeof(plan_step));
    plan_step *s = &p->steps[p->n++];
    size_t na = gf_argv_len(argv);
    s->argv = gf_malloc((na + 1) * sizeof(char *));
    for (size_t i = 0; i < na; i++)
        s->argv[i] = gf_strdup(argv[i]);
    s->argv[na] = NULL;
    s->cwd = cwd;
    s->desc = gf_strdup(desc);
    s->is_test = is_test;
}

static void plan_addf(build_plan *p, const char *cwd, const char *desc,
                      bool is_test, ...)
    __attribute__((sentinel));
static void plan_addf(build_plan *p, const char *cwd, const char *desc,
                      bool is_test, ...)
{
    /* variadic argv terminated by NULL */
    const char *argv[64];
    size_t n = 0;
    va_list ap;
    va_start(ap, is_test);
    const char *a = va_arg(ap, const char *);
    while (a && n < 63) {
        argv[n++] = a;
        a = va_arg(ap, const char *);
    }
    va_end(ap);
    argv[n] = NULL;
    plan_addv(p, argv, cwd, desc, is_test);
}

/* ---------------------------------------------------- main pipeline */

int gf_build_package(const gf_config *cfg, gf_db *db, gf_resolved *res,
                     const gf_build_opts *opts, gf_build_result *out)
{
    memset(out, 0, sizeof(*out));
    gf_build_opts defopts = { 0 };
    const gf_build_opts *o = opts ? opts : &defopts;

    char *name = gf_pkgref_name(&res->ref);
    if (!name) {
        gf_log(GF_LOG_ERROR, "cannot derive package name");
        return -1;
    }

    /* ---- build directory ---- */
    char version_dir[256];
    snprintf(version_dir, sizeof(version_dir), "%s",
             res->is_edge ? "edge" : (res->version ? res->version : "unknown"));
    const char *src_tag = res->src.commit && res->src.commit[0]
        ? res->src.commit : "nocommit";
    char build_dir[4096];
    snprintf(build_dir, sizeof(build_dir), "%s/builds/%s/%s/%.12s",
             cfg->state_dir, name, version_dir, src_tag);
    if (gf_fs_exists(build_dir) && o->force) {
        gf_fs_rm_rf(build_dir);
    }
    if (gf_fs_mkdir_p(build_dir) != 0) {
        free(name);
        return -1;
    }

    char *srcdir = gf_path_join(build_dir, "source");
    char *builddir = gf_path_join(build_dir, "build");
    char *staging = gf_path_join(build_dir, "staging");
    char *depsroot = gf_path_join(build_dir, "deps");
    char *sbroot = gf_path_join(build_dir, "root");
    char *logsdir = gf_path_join(build_dir, "logs");
    if (gf_fs_mkdir_p(builddir) != 0 || gf_fs_mkdir_p(staging) != 0) {
        free(srcdir); free(builddir); free(staging); free(depsroot);
        free(sbroot); free(logsdir); free(name);
        return -1;
    }

    /* build log tee */
    char *buildlog = gf_path_join(logsdir, "build.log");
    gf_fs_mkdir_p(logsdir);
    gf_log_set_tee(buildlog);

    gf_log(GF_LOG_INFO, "building %s %s (%s)", name, res->version,
           gf_src_status_name(res->src.status));

    /* ---- 1. acquire source ---- */
    if (gf_fs_exists(srcdir)) {
        gf_log(GF_LOG_INFO, "using existing source in %s", srcdir);
    } else if (gf_source_acquire(cfg, res, srcdir) != 0) {
        gf_log(GF_LOG_ERROR, "source acquisition failed");
        goto fail_1;
    }

    /* ---- 2. recipe + detection ---- */
    gf_recipe *recipe = gf_recipe_load(srcdir);
    if (!recipe) {
        gf_log(GF_LOG_ERROR, "invalid gitfull.toml in %s", srcdir);
        goto fail_1;
    }
    const char *detect_root = srcdir;
    if (recipe->subdir) {
        char *sub = gf_path_join(srcdir, recipe->subdir);
        if (gf_fs_is_dir(sub))
            detect_root = sub;
        else {
            gf_log(GF_LOG_ERROR, "recipe subdir does not exist: %s", sub);
            free(sub);
            goto fail_2;
        }
    }
    gf_bsys bsys;
    if (recipe->build_system) {
        if (gf_str_ieq(recipe->build_system, "cmake"))
            bsys = GF_BSYS_CMAKE;
        else if (gf_str_ieq(recipe->build_system, "meson"))
            bsys = GF_BSYS_MESON;
        else if (gf_str_ieq(recipe->build_system, "make"))
            bsys = GF_BSYS_MAKE;
        else if (gf_str_ieq(recipe->build_system, "autotools"))
            bsys = GF_BSYS_AUTOTOOLS;
        else if (gf_str_ieq(recipe->build_system, "cargo"))
            bsys = GF_BSYS_CARGO;
        else if (gf_str_ieq(recipe->build_system, "go"))
            bsys = GF_BSYS_GO;
        else if (gf_str_ieq(recipe->build_system, "node"))
            bsys = GF_BSYS_NODE;
        else if (gf_str_ieq(recipe->build_system, "python"))
            bsys = GF_BSYS_PYTHON;
        else if (gf_str_ieq(recipe->build_system, "vala"))
            bsys = GF_BSYS_VALA;
        else {
            bsys = GF_BSYS_CUSTOM;
        }
    } else {
        bsys = gf_bsys_detect(detect_root);
        if (bsys == GF_BSYS_NONE) {
            gf_log(GF_LOG_ERROR,
                   "cannot detect a build system in %s (no CMakeLists.txt, "
                   "meson.build, Makefile, configure, Cargo.toml, go.mod, "
                   "pyproject.toml, setup.py or package.json) — add a "
                   "gitfull.toml recipe",
                   detect_root);
            goto fail_2;
        }
    }
    gf_log(GF_LOG_INFO, "build system: %s (%s)", gf_bsys_name(bsys),
           gf_bsys_description(bsys));

    /* ---- 3. toolchain ---- */
    gf_toolchain tc;
    if (gf_toolchain_probe(cfg, gf_bsys_name(bsys), &tc) != 0) {
        gf_log(GF_LOG_ERROR, "toolchain probing failed");
        goto fail_2;
    }
    /* required tools present? */
    {
        const char *const *need = gf_bsys_tools(bsys);
        for (int i = 0; need[i]; i++) {
            if (!gf_toolchain_get(&tc, need[i])) {
                gf_log(GF_LOG_ERROR,
                       "required tool '%s' for build system '%s' is not "
                       "available on this host (bootstrap mode)",
                       need[i], gf_bsys_name(bsys));
                gf_toolchain_free(&tc);
                goto fail_2;
            }
        }
    }

    /* ---- 4. dependency payloads ---- */
    {
        /* deps are re-assembled from the store on every build: a stale
         * tree from an earlier attempt would make hardlink assembly fail */
        if (gf_fs_exists(depsroot))
            gf_fs_rm_rf(depsroot);
        char *depsusr = gf_path_join(depsroot, "usr");
        gf_fs_mkdir_p(depsusr);
        for (size_t i = 0; i < o->ndeps; i++) {
            char *files = gf_store_files_path(cfg->state_dir, o->deps[i].name,
                                              o->deps[i].version,
                                              o->deps[i].build_id);
            if (!files || !gf_fs_is_dir(files)) {
                gf_log(GF_LOG_ERROR, "dependency payload missing in store: %s",
                       o->deps[i].name);
                free(files);
                free(depsusr);
                gf_toolchain_free(&tc);
                goto fail_2;
            }
            /* overlay the dep payload tree into deps/usr (root-relative) */
            char *dst = gf_path_join(depsusr, "local");
            (void)dst;
            /* payload paths are root-relative (usr/local/...): merge at
             * deps root so /deps/usr/local/... appears in the sandbox */
            if (gf_fs_hardlink_tree(files, depsroot) != 0) {
                gf_log(GF_LOG_ERROR, "dependency assembly failed: %s",
                       o->deps[i].name);
                free(files);
                free(depsusr);
                gf_toolchain_free(&tc);
                goto fail_2;
            }
            free(files);
        }
        free(depsusr);
    }

    /* ---- 5. sandbox preparation ---- */
    gf_sandbox_paths sp;
    memset(&sp, 0, sizeof(sp));
    sp.root = sbroot;
    sp.toolchain = tc.dir;
    sp.src = srcdir;
    sp.build = builddir;
    sp.staging = staging;
    sp.deps = gf_fs_is_dir(depsroot) ? depsroot : NULL;
    /* NOTE: system_dirs must stay valid for the lifetime of the sandbox
     * runs — tc (the toolchain) is freed only after the build completes. */
    sp.system_dirs = (const char *const *)tc.system_dirs;
    if (gf_sandbox_prepare_root(&sp, cfg->prefix, false) != 0) {
        gf_toolchain_free(&tc);
        goto fail_2;
    }

    gf_sandbox_level level = GF_SANDBOX_USERNS;
    if (gf_str_ieq(cfg->sandbox_level, "none"))
        level = GF_SANDBOX_NONE;
    else if (gf_str_ieq(cfg->sandbox_level, "chroot"))
        level = GF_SANDBOX_CHROOT;
    else if (level == GF_SANDBOX_USERNS && !gf_sandbox_userns_available()) {
        gf_log(GF_LOG_WARN,
               "user namespaces unavailable; falling back to sandbox=none "
               "(builds will run with environment isolation only)");
        level = GF_SANDBOX_NONE;
    }

    /* ---- 6. environment (hermetic; no host variables leak in) ---- */
    /* Pure install prefix (relative to the install root): build systems get
     * DESTDIR semantics, so they must stage under staging/<pure-prefix>.
     * cfg->prefix may be ROOT-JOINED (default with --root); strip the root
     * but KEEP the absolute form (/usr/local) — DESTDIR + absolute prefix
     * is the standard staging convention. */
    char prefixarg[1024];
    {
        const char *pure = cfg->prefix;
        size_t rootlen = strlen(cfg->root);
        if (rootlen && strncmp(cfg->prefix, cfg->root, rootlen) == 0) {
            pure = cfg->prefix + rootlen;
            if (*pure == '\0')
                pure = ""; /* prefix == root itself */
            /* else: keep the leading '/' (absolute prefix) */
        }
        snprintf(prefixarg, sizeof(prefixarg), "%s", pure);
    }
    char **env = env_new();
    size_t nenv = 0;
    {
        int64_t sde = commit_epoch(cfg, res);
        if (sde <= 0)
            sde = 0;
        char buf[2048];
        env_push(&env, &nenv, "PATH=/toolchain/bin:/usr/bin:/bin");
        snprintf(buf, sizeof(buf), "HOME=/home/build");
        env_push(&env, &nenv, buf);
        env_push(&env, &nenv, "TMPDIR=/tmp");
        env_push(&env, &nenv, "TERM=dumb");
        env_push(&env, &nenv, "TZ=UTC");
        env_push(&env, &nenv, "LC_ALL=C.UTF-8");
        env_push(&env, &nenv, "LANG=C.UTF-8");
        snprintf(buf, sizeof(buf), "SOURCE_DATE_EPOCH=%lld",
                 (long long)sde);
        env_push(&env, &nenv, buf);
        env_push(&env, &nenv, "MAKEFLAGS=");

        /* dependency discovery env (only when deps exist). These MUST be
         * the in-SANDBOX paths (/deps/...), not host paths: the build only
         * sees the bind-mounted view after pivot_root. Dependencies were
         * staged with the same prefix, so use the pure prefix here. */
        if (gf_fs_is_dir(depsroot)) {
            char dbuf[3072];
            snprintf(dbuf, sizeof(dbuf), "CPATH=/deps%s/include", prefixarg);
            env_push(&env, &nenv, dbuf);
            snprintf(dbuf, sizeof(dbuf), "LIBRARY_PATH=/deps%s/lib", prefixarg);
            env_push(&env, &nenv, dbuf);
            snprintf(dbuf, sizeof(dbuf),
                     "PKG_CONFIG_PATH=/deps%s/lib/pkgconfig:"
                     "/deps%s/share/pkgconfig", prefixarg, prefixarg);
            env_push(&env, &nenv, dbuf);
            snprintf(dbuf, sizeof(dbuf), "CMAKE_PREFIX_PATH=/deps%s",
                     prefixarg);
            env_push(&env, &nenv, dbuf);
        }
        /* rpath strategy: package libs are found relative to the binary and
         * via the ACTIVATED prefix (cfg->prefix, root-joined under --root;
         * the real filesystem location after installation). */
        snprintf(buf, sizeof(buf),
                 "CFLAGS=-O2 -I/deps%s/include -ffile-prefix-map=/src=. "
                 "-fdebug-prefix-map=/src=. -Wdate-time", prefixarg);
        env_push(&env, &nenv, buf);
        snprintf(buf, sizeof(buf),
                 "CXXFLAGS=-O2 -I/deps%s/include -ffile-prefix-map=/src=. "
                 "-fdebug-prefix-map=/src=. -Wdate-time", prefixarg);
        env_push(&env, &nenv, buf);
        snprintf(buf, sizeof(buf),
                 "LDFLAGS=-L/deps%s/lib -Wl,-rpath,\\$ORIGIN/../lib "
                 "-Wl,-rpath,%s/lib",
                 prefixarg, cfg->prefix);
        env_push(&env, &nenv, buf);
        /* recipe env (applied last so it can override) */
        for (size_t i = 0; recipe->env && recipe->env[i]; i++)
            env_push(&env, &nenv, recipe->env[i]);
    }

    /* ---- 7. build plan ---- */
    build_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.system = gf_strdup(gf_bsys_name(bsys));
    {
        char jobsarg[32];
        snprintf(jobsarg, sizeof(jobsarg), "%d",
                 recipe->jobs > 0 ? recipe->jobs : cfg->jobs);

        if (recipe->nsteps > 0 || recipe->ninstall > 0 || recipe->ntest > 0) {
            /* recipe-provided steps override everything */
            for (size_t i = 0; i < recipe->nsteps; i++) {
                plan_add(&plan, recipe->steps[i].argv,
                         recipe->steps[i].cwd ? recipe->steps[i].cwd : "build",
                         recipe->steps[i].desc ? recipe->steps[i].desc
                                               : "recipe step", false);
            }
            for (size_t i = 0; i < recipe->ninstall; i++) {
                plan_add(&plan, recipe->install_steps[i].argv,
                         recipe->install_steps[i].cwd
                             ? recipe->install_steps[i].cwd : "build",
                         recipe->install_steps[i].desc
                             ? recipe->install_steps[i].desc : "install", false);
            }
            for (size_t i = 0; i < recipe->ntest; i++) {
                plan_add(&plan, recipe->test_steps[i].argv,
                         recipe->test_steps[i].cwd ? recipe->test_steps[i].cwd
                                                   : "build",
                         recipe->test_steps[i].desc
                             ? recipe->test_steps[i].desc : "test", true);
            }
        } else {
            switch (bsys) {
            case GF_BSYS_CMAKE:
                plan_addf(&plan, "build", "cmake configure", false,
                          "cmake", "-S", "/src", "-B", "/build", "-G", "Ninja",
                          "-DCMAKE_BUILD_TYPE=Release",
                          "-DCMAKE_INSTALL_PREFIX=", prefixarg, NULL);
                /* -DCMAKE_INSTALL_PREFIX=<prefix> as a single argv entry */
                {
                    char cip[1100];
                    snprintf(cip, sizeof(cip), "-DCMAKE_INSTALL_PREFIX=%s",
                             prefixarg);
                    char **av = plan.steps[plan.n - 1].argv;
                    size_t al = gf_argv_len((const char *const *)av);
                    free(av[al - 2]);
                    free(av[al - 1]);
                    av[al - 2] = gf_strdup(cip);
                    av[al - 1] = NULL;
                }
                plan_addf(&plan, "build", "ninja build", false,
                          "ninja", "-C", "/build", "-j", jobsarg, NULL);
                plan_addf(&plan, "build", "install to staging", false,
                          "ninja", "-C", "/build", "install", NULL);
                plan_addf(&plan, "build", "ctest", true,
                          "ctest", "--test-dir", "/build", "--output-on-failure",
                          NULL);
                break;
            case GF_BSYS_VALA:
            case GF_BSYS_MESON:
                plan_addf(&plan, "build", "meson setup", false,
                          "meson", "setup", "/build", "/src",
                          "--prefix", prefixarg, "--buildtype", "release",
                          "-Db_lto=true", NULL);
                plan_addf(&plan, "build", "ninja build", false,
                          "ninja", "-C", "/build", "-j", jobsarg, NULL);
                plan_addf(&plan, "build", "install to staging", false,
                          "sh", "-c", "DESTDIR=/staging ninja -C /build install",
                          NULL);
                plan_addf(&plan, "build", "meson test", true,
                          "meson", "test", "-C", "/build", NULL);
                break;
            case GF_BSYS_AUTOTOOLS: {
                char conf[2048];
                char *qpfx = gf_str_shell_quote(prefixarg);
                snprintf(conf, sizeof(conf), "/src/configure --prefix=%s",
                         qpfx);
                free(qpfx);
                plan_addf(&plan, "build", "configure", false,
                          "sh", "-c", conf, NULL);
                plan_addf(&plan, "build", "make", false,
                          "make", "-j", jobsarg, NULL);
                plan_addf(&plan, "build", "install to staging", false,
                          "sh", "-c", "DESTDIR=/staging make install", NULL);
                plan_addf(&plan, "build", "make check", true,
                          "sh", "-c", "make check || make test", NULL);
                break;
            }
            case GF_BSYS_MAKE:
                plan_addf(&plan, "src", "make", false,
                          "make", "-j", jobsarg, NULL);
                plan_addf(&plan, "src", "install to staging", false,
                          "sh", "-c", "make DESTDIR=/staging install", NULL);
                {
                    /* Pass BOTH spellings: PREFIX= (upper) overrides the
                     * conventional `PREFIX ?= /usr/local` default, prefix=
                     * (lower) catches packages that use the lowercase form.
                     * Command-line assignments beat ?= in GNU and BSD make. */
                    char mk[4096];
                    char *qpfx = gf_str_shell_quote(prefixarg);
                    snprintf(mk, sizeof(mk),
                             "make DESTDIR=/staging PREFIX=%s prefix=%s install",
                             qpfx, qpfx);
                    free(qpfx);
                    free(plan.steps[plan.n - 1].argv[2]);
                    plan.steps[plan.n - 1].argv[2] = gf_strdup(mk);
                }
                plan_addf(&plan, "src", "make check", true,
                          "sh", "-c", "make check || make test", NULL);
                break;
            case GF_BSYS_CARGO:
                plan_addf(&plan, "src", "cargo build", false,
                          "cargo", "build", "--release", "--offline", NULL);
                plan_addf(&plan, "src", "cargo install to staging", false,
                          "cargo", "install", "--path", "/src",
                          "--root", "/staging/usr/local", "--offline",
                          "--force", NULL);
                plan_addf(&plan, "src", "cargo test", true,
                          "cargo", "test", "--offline", NULL);
                break;
            case GF_BSYS_GO: {
                char gop[256];
                snprintf(gop, sizeof(gop), "/build/%s", name);
                plan_addf(&plan, "src", "go build", false,
                          "go", "build", "-o", gop, "-trimpath", "./...", NULL);
                plan_addf(&plan, "src", "stage binary", false,
                          "install", "-D", gop,
                          "/staging/usr/local/bin/x", NULL);
                {
                    char dstbin[2048];
                    snprintf(dstbin, sizeof(dstbin), "/staging%s/bin/%s",
                             prefixarg, name);
                    free(plan.steps[plan.n - 1].argv[3]);
                    plan.steps[plan.n - 1].argv[3] = gf_strdup(dstbin);
                }
                plan_addf(&plan, "src", "go test", true,
                          "go", "test", "./...", NULL);
                break;
            }
            case GF_BSYS_PYTHON:
                plan_addf(&plan, "src", "pip wheel (build)", false,
                          "python3", "-m", "pip", "wheel", "--no-deps",
                          "--wheel-dir", "/build", "/src", NULL);
                plan_addf(&plan, "src", "pip install to staging", false,
                          "sh", "-c",
                          "python3 -m pip install --no-deps --force-reinstall "
                          "--prefix /staging/usr/local /build/*.whl",
                          NULL);
                plan_addf(&plan, "src", "pytest", true,
                          "sh", "-c", "cd /src && python3 -m pytest || true",
                          NULL);
                break;
            case GF_BSYS_NODE:
                plan_addf(&plan, "src", "npm install", false,
                          "npm", "ci", "--ignore-scripts", "--prefix", "/src",
                          NULL);
                plan_addf(&plan, "src", "npm pack", false,
                          "npm", "pack", "--pack-destination", "/build",
                          NULL);
                plan_addf(&plan, "src", "stage package", false,
                          "sh", "-c",
                          "mkdir -p /staging/usr/local/lib/node_modules && "
                          "tar -xzf /build/*.tgz -C "
                          "/staging/usr/local/lib/node_modules",
                          NULL);
                plan_addf(&plan, "src", "npm test", true,
                          "npm", "test", "--prefix", "/src", NULL);
                break;
            case GF_BSYS_CUSTOM:
            case GF_BSYS_NONE:
            default:
                gf_log(GF_LOG_ERROR,
                       "custom build system requires gitfull.toml steps");
                goto fail_3;
            }
        }
    }

    /* ---- 8. run the plan in the sandbox ---- */
    {
        char *steps_jsonl = gf_path_join(logsdir, "steps.jsonl");
        FILE *sj = fopen(steps_jsonl, "w");
        free(steps_jsonl);
        if (sj)
            fclose(sj);
    }
    int rc_build = 0, rc_test = 0;
    bool run_tests = o->skip_tests ? false
        : (recipe->run_tests_set ? recipe->run_tests : cfg->run_tests);
    bool netns_used = !gf_str_ieq(cfg->sandbox_network, "fetch");
    for (size_t i = 0; i < plan.n; i++) {
        plan_step *s = &plan.steps[i];
        if (s->is_test && !run_tests) {
            gf_log(GF_LOG_INFO, "skipping tests (disabled)");
            break;
        }
        gf_log(GF_LOG_INFO, "step: %s", s->desc);
        gf_strbuf outb, errb;
        gf_strbuf_init(&outb);
        gf_strbuf_init(&errb);

        gf_sandbox_run src;
        memset(&src, 0, sizeof(src));
        src.level = level;
        src.netns = netns_used;
        src.seccomp = cfg->sandbox_seccomp;
        src.cgroup_memory_mb = cfg->memory_limit_mb;
        src.cgroup_cpu_pct = cfg->cpu_quota_percent;
        src.timeout_ms = o->timeout_ms ? o->timeout_ms
                           : (uint64_t)2 * 60 * 60 * 1000; /* 2h per step */
        src.cwd_in = strcmp(s->cwd, "src") == 0 ? "/src" : "/build";
        src.envp = env;
        src.out = &outb;
        src.err = &errb;

        int status = -1;
        bool timed_out = false;
        int rc = gf_sandbox_exec(&sp, &src, (const char *const *)s->argv,
                                 &status, &timed_out);
        /* log the output */
        {
            char *steps_jsonl = gf_path_join(logsdir, "steps.jsonl");
            FILE *sj = fopen(steps_jsonl, "a");
            if (sj) {
                gf_json *jrec = gf_json_new_object();
                gf_json_object_set(jrec, "desc", gf_json_new_string(s->desc));
                gf_json *av = gf_json_new_array();
                for (size_t k = 0; s->argv[k]; k++)
                    gf_json_array_push(av, gf_json_new_string(s->argv[k]));
                gf_json_object_set(jrec, "argv", av);
                gf_json_object_set(jrec, "status", gf_json_new_int(status));
                gf_json_object_set(jrec, "timed_out", gf_json_new_bool(timed_out));
                gf_json_object_set(jrec, "stdout", gf_json_new_string(gf_strbuf_str(&outb)));
                gf_json_object_set(jrec, "stderr", gf_json_new_string(gf_strbuf_str(&errb)));
                char *line = gf_json_dump(jrec);
                gf_json_free(jrec);
                fprintf(sj, "%s\n", line);
                free(line);
                fclose(sj);
            }
            free(steps_jsonl);
        }
        if (outb.len > 0 && gf_log_level() <= GF_LOG_DEBUG) {
            LOGD("stdout: %s", gf_strbuf_str(&outb));
        }
        if (errb.len > 0)
            gf_log(GF_LOG_DEBUG, "stderr: %s", gf_strbuf_str(&errb));
        gf_strbuf_free(&outb);
        gf_strbuf_free(&errb);
        if (rc != 0) {
            gf_log(GF_LOG_ERROR, "step '%s' could not run (sandbox failure)",
                   s->desc);
            rc_build = -1;
            break;
        }
        if (timed_out) {
            gf_log(GF_LOG_ERROR, "step '%s' timed out", s->desc);
            rc_build = -1;
            break;
        }
        if (status != 0) {
            if (s->is_test) {
                gf_log(GF_LOG_ERROR, "tests failed (step '%s', exit %d)",
                       s->desc, status);
                rc_test = 1;
            } else {
                gf_log(GF_LOG_ERROR, "step '%s' failed (exit %d)", s->desc,
                       status);
                rc_build = 1;
            }
            break;
        }
    }
    if (rc_build != 0 || rc_test != 0) {
        env_free(env, nenv);
        plan_free(&plan);
        gf_toolchain_free(&tc);
        gf_log_close_tee();
        int ret = rc_test ? GF_EXIT_TEST : GF_EXIT_BUILD;
        free(srcdir); free(builddir); free(staging); free(depsroot);
        free(sbroot); free(logsdir); free(name); free(buildlog);
        gf_recipe_free(recipe);
        return ret;
    }

    /* ---- 9. validate staging ---- */
    {
        size_t nf = 0;
        char **entries = NULL;
        if (gf_fs_list_dir(staging, &entries, &nf) != 0 || nf == 0) {
            gf_log(GF_LOG_ERROR,
                   "staging is empty — the build produced no files");
            env_free(env, nenv);
            plan_free(&plan);
            gf_toolchain_free(&tc);
            gf_log_close_tee();
            free(srcdir); free(builddir); free(staging); free(depsroot);
            free(sbroot); free(logsdir); free(name); free(buildlog);
            gf_recipe_free(recipe);
            return GF_EXIT_BUILD;
        }
        for (size_t i = 0; i < nf; i++)
            free(entries[i]);
        free(entries);
    }

    /* ---- 10. package metadata + identity ---- */
    gf_pkgmeta *meta = gf_calloc(1, sizeof(gf_pkgmeta));
    {
        /* SOURCE_DATE_EPOCH for the package = commit time (or 0) */
        int64_t sde = commit_epoch(cfg, res);
        char *bi = NULL;
        {
            gf_json *id = gf_json_new_object();
            gf_json_object_set(id, "source_url",
                               gf_json_new_string(res->src.repo_url));
            gf_json_object_set(id, "source_ref",
                               gf_json_new_string(res->src.ref));
            gf_json_object_set(id, "commit",
                               gf_json_new_string(res->src.commit ? res->src.commit : ""));
            gf_json_object_set(id, "source_sha256",
                               gf_json_new_string(res->src.archive_sha ? res->src.archive_sha : ""));
            gf_json_object_set(id, "source_status",
                               gf_json_new_string(gf_src_status_name(res->src.status)));
            gf_json_object_set(id, "build_system", gf_json_new_string(plan.system));
            gf_json_object_set(id, "sandbox", gf_json_new_string(gf_sandbox_level_name(level)));
            gf_json_object_set(id, "network", gf_json_new_string(netns_used ? "none" : "fetch"));
            gf_json_object_set(id, "jobs", gf_json_new_int(recipe->jobs > 0 ? recipe->jobs : cfg->jobs));
            gf_json_object_set(id, "source_date_epoch", gf_json_new_int(sde));
            gf_json_object_set(id, "architecture", gf_json_new_string(cfg->arch));
            gf_json_object_set(id, "kernel", gf_json_new_string("linux"));
            gf_json_object_set(id, "recipe", gf_json_new_string(recipe->present ? "gitfull.toml" : "autodetected"));
            gf_json *fl = gf_json_new_array();
            static const char *flags[] = { "CFLAGS=-O2 -ffile-prefix-map=/src=.",
                                           "LDFLAGS rpath=$ORIGIN/../lib," };
            for (int i = 0; i < 2; i++)
                gf_json_array_push(fl, gf_json_new_string(flags[i]));
            gf_json_object_set(id, "flags", fl);
            bi = gf_json_dump(id);
            gf_json_free(id);
        }
        meta->name = gf_strdup(recipe->name ? recipe->name : name);
        meta->version = gf_strdup(res->version ? res->version : "0");
        meta->arch = gf_strdup(cfg->arch);
        meta->forge = gf_strdup(res->src.forge);
        meta->repo_url = gf_strdup(res->src.repo_url);
        meta->commit = gf_strdup(res->src.commit);
        meta->source_sha = gf_strdup(res->src.archive_sha);
        meta->source_status = gf_strdup(gf_src_status_name(res->src.status));
        meta->description = gf_strdup(res->src.description ? res->src.description
                                       : (recipe->description ? recipe->description : ""));
        meta->license = gf_strdup(res->src.license ? res->src.license
                              : (recipe->license ? recipe->license : ""));
        meta->build_identity = bi;
        meta->toolchain_id = NULL; /* filled after manifest */
        /* deps from recipe + resolved graph info */
        meta->ndeps = recipe->ndeps + recipe->nbdeps;
        meta->dep_names = gf_malloc((meta->ndeps ? meta->ndeps : 1) * sizeof(char *));
        meta->dep_specs = gf_malloc((meta->ndeps ? meta->ndeps : 1) * sizeof(char *));
        meta->dep_kinds = gf_malloc((meta->ndeps ? meta->ndeps : 1) * sizeof(char *));
        size_t w = 0;
        for (size_t i = 0; i < recipe->ndeps; i++) {
            meta->dep_names[w] = gf_strdup(recipe->dep_names[i]);
            meta->dep_specs[w] = gf_strdup(recipe->dep_specs[i]);
            meta->dep_kinds[w] = gf_strdup("run");
            w++;
        }
        for (size_t i = 0; i < recipe->nbdeps; i++) {
            meta->dep_names[w] = gf_strdup(recipe->bdep_names[i]);
            meta->dep_specs[w] = gf_strdup(recipe->bdep_specs[i]);
            meta->dep_kinds[w] = gf_strdup("build");
            w++;
        }
        meta->ndeps = w;

        /* ---- 11. create the package ---- */
        char *out_pkg = gf_path_join(build_dir, "pkg.gfpkg");
        char *build_id = gf_package_create(staging, meta, out_pkg,
                                           (uint64_t)(sde < 0 ? 0 : sde),
                                           NULL, NULL);
        if (!build_id) {
            gf_log(GF_LOG_ERROR, "package creation failed");
            free(out_pkg);
            gf_pkgmeta_free_heap(meta);
            env_free(env, nenv);
            plan_free(&plan);
            gf_toolchain_free(&tc);
            gf_log_close_tee();
            free(srcdir); free(builddir); free(staging); free(depsroot);
            free(sbroot); free(logsdir); free(name); free(buildlog);
            gf_recipe_free(recipe);
            return GF_EXIT_BUILD;
        }
        free(out_pkg);
        meta->build_id = gf_strdup(build_id);
        meta->pkg_sha = gf_strdup(build_id);

        /* toolchain id: hash of the toolchain manifest */
        {
            gf_json *tm = gf_toolchain_manifest(&tc);
            char *dump = gf_json_dump(tm);
            gf_json_free(tm);
            meta->toolchain_id = gf_sha256_buf_hex(dump, strlen(dump));
            free(dump);
        }

        /* ---- 12. verify + store ---- */
        {
            char *dir = gf_path_join_multi(cfg->state_dir, "store", meta->name,
                                           meta->version, build_id, NULL);
            char *pkgp = gf_path_join(dir, "pkg.gfpkg");
            if (gf_fs_mkdir_p(dir) != 0) {
                free(dir);
                free(pkgp);
                free(build_id);
                gf_pkgmeta_free_heap(meta);
                env_free(env, nenv);
                plan_free(&plan);
                gf_toolchain_free(&tc);
                gf_log_close_tee();
                free(srcdir); free(builddir); free(staging); free(depsroot);
                free(sbroot); free(logsdir); free(name); free(buildlog);
                gf_recipe_free(recipe);
                return -1;
            }
            char *srcfile = gf_path_join(build_dir, "pkg.gfpkg");
            bool fresh_store = !gf_fs_exists(pkgp);
            if (rename(srcfile, pkgp) != 0)
                gf_fs_copy_file(srcfile, pkgp, 0644);
            unlink(srcfile);
            free(srcfile);
            /* payload files for activation (skip when this build-id is
             * already materialized: content-addressed idempotence) */
            char *filesd = gf_path_join(dir, "files");
            bool have_payload = false;
            {
                size_t nfl = 0;
                char **fl = NULL;
                if (gf_fs_list_dir(filesd, &fl, &nfl) == 0) {
                    have_payload = nfl > 0;
                    for (size_t k = 0; k < nfl; k++)
                        free(fl[k]);
                    free(fl);
                }
            }
            if (!fresh_store && have_payload) {
                /* same build-id already present: nothing to extract */
            } else if (gf_fs_mkdir_p(filesd) == 0 &&
                       gf_package_extract_payload(pkgp, filesd) != 0) {
                gf_log(GF_LOG_ERROR, "payload extraction failed");
                free(dir);
                free(pkgp);
                free(filesd);
                free(build_id);
                gf_pkgmeta_free_heap(meta);
                env_free(env, nenv);
                plan_free(&plan);
                gf_toolchain_free(&tc);
                gf_log_close_tee();
                free(srcdir); free(builddir); free(staging); free(depsroot);
                free(sbroot); free(logsdir); free(name); free(buildlog);
                gf_recipe_free(recipe);
                return -1;
            }
            /* verify the stored package hashes correctly */
            if (gf_package_verify(pkgp, filesd) != 0) {
                gf_log(GF_LOG_ERROR, "package verification failed");
                gf_fs_rm_rf(dir);
                free(dir);
                free(pkgp);
                free(filesd);
                free(build_id);
                gf_pkgmeta_free_heap(meta);
                env_free(env, nenv);
                plan_free(&plan);
                gf_toolchain_free(&tc);
                gf_log_close_tee();
                free(srcdir); free(builddir); free(staging); free(depsroot);
                free(sbroot); free(logsdir); free(name); free(buildlog);
                gf_recipe_free(recipe);
                return -1;
            }
            free(filesd);

            out->build_id = build_id;
            out->pkg_path = pkgp;
            out->meta = gf_pkgmeta_copy(meta);
            out->build_dir = gf_strdup(build_dir);
            free(dir);
        }

        /* identity.json in the logs */
        {
            char *idp = gf_path_join(logsdir, "identity.json");
            char *dump = gf_json_dump_pretty(gf_pkgmeta_to_json(meta));
            gf_fs_write_file_atomic(idp, dump, strlen(dump));
            free(dump);
            free(idp);
        }

        gf_log(GF_LOG_INFO, "built %s %s: build-id %s", meta->name, meta->version,
               build_id);
        gf_pkgmeta_free_heap(meta);
    }

    env_free(env, nenv);
    plan_free(&plan);
    gf_toolchain_free(&tc);
    gf_recipe_free(recipe);
    gf_log_close_tee();
    free(srcdir);
    free(builddir);
    free(staging);
    free(depsroot);
    free(sbroot);
    free(logsdir);
    free(name);
    free(buildlog);
    (void)db;
    return 0;

fail_3:
    env_free(env, nenv);
    plan_free(&plan);
    gf_toolchain_free(&tc);
fail_2:
    gf_recipe_free(recipe);
fail_1:
    gf_log_close_tee();
    free(srcdir);
    free(builddir);
    free(staging);
    free(depsroot);
    free(sbroot);
    free(logsdir);
    free(name);
    free(buildlog);
    return -1;
}
