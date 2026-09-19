/* recipe.c — gitfull.toml recipe loading (override/extension mechanism). */
#include "recipe.h"

#include "common.h"
#include "toml.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* parse [[build.steps]]-style arrays of tables with argv/cwd/desc */
static int steps_from_json(const gf_json *arr, gf_recipe_step **out,
                           size_t *n)
{
    *out = NULL;
    *n = 0;
    if (!arr || arr->type != GF_JSON_ARRAY)
        return 0;
    size_t len = gf_json_len(arr);
    if (len == 0)
        return 0;
    gf_recipe_step *steps = gf_calloc(len, sizeof(gf_recipe_step));
    for (size_t i = 0; i < len; i++) {
        const gf_json *item = gf_json_at(arr, i);
        if (!item || item->type != GF_JSON_OBJECT) {
            gf_log(GF_LOG_ERROR, "recipe: steps entries must be tables");
            goto fail;
        }
        const gf_json *argvj = gf_json_get(item, "argv");
        if (!argvj || argvj->type != GF_JSON_ARRAY ||
            gf_json_len(argvj) == 0) {
            gf_log(GF_LOG_ERROR,
                   "recipe: every step needs a non-empty argv array");
            goto fail;
        }
        for (size_t k = 0; k < gf_json_len(argvj); k++) {
            if (!gf_json_str(gf_json_at(argvj, k))) {
                gf_log(GF_LOG_ERROR, "recipe: argv entries must be strings");
                goto fail;
            }
        }
        size_t alen = gf_json_len(argvj);
        steps[*n].argv = gf_malloc((alen + 1) * sizeof(char *));
        for (size_t k = 0; k < alen; k++)
            steps[*n].argv[k] = gf_strdup(gf_json_str(gf_json_at(argvj, k)));
        steps[*n].argv[alen] = NULL;
        const char *cwd = gf_json_str(gf_json_get(item, "cwd"));
        if (cwd && strcmp(cwd, "src") != 0 && strcmp(cwd, "build") != 0 &&
            !gf_path_component_ok(cwd)) {
            gf_log(GF_LOG_ERROR, "recipe: unsafe cwd in step: %s", cwd);
            goto fail;
        }
        steps[*n].cwd = cwd ? gf_strdup(cwd) : NULL;
        const char *desc = gf_json_str(gf_json_get(item, "desc"));
        steps[*n].desc = desc ? gf_strdup(desc) : NULL;
        (*n)++;
    }
    *out = steps;
    return 0;
fail:
    for (size_t i = 0; i < *n; i++) {
        if (steps[i].argv) {
            for (size_t k = 0; steps[i].argv[k]; k++)
                free(steps[i].argv[k]);
            free(steps[i].argv);
        }
        free(steps[i].cwd);
        free(steps[i].desc);
    }
    free(steps);
    return -1;
}

/* deps tables: name = "spec" (spec optional -> "*") */
static int deps_from_json(const gf_json *tbl, char ***names, char ***specs,
                          size_t *n)
{
    *names = *specs = NULL;
    *n = 0;
    if (!tbl || tbl->type != GF_JSON_OBJECT)
        return 0;
    size_t len = gf_json_len(tbl);
    if (len == 0)
        return 0;
    char **nm = gf_malloc(len * sizeof(char *));
    char **sp = gf_malloc(len * sizeof(char *));
    for (size_t i = 0; i < len; i++) {
        const char *key = gf_json_key(tbl, i);
        if (!gf_path_component_ok(key)) {
            gf_log(GF_LOG_ERROR, "recipe: invalid dependency name: %s", key);
            goto fail;
        }
        const gf_json *v = gf_json_val(tbl, i);
        const char *spec = "*";
        if (v && v->type == GF_JSON_STRING)
            spec = gf_json_str(v);
        else if (v && v->type == GF_JSON_OBJECT)
            spec = "*"; /* structured dep: accept, use any */
        else if (v && v->type != GF_JSON_NULL) {
            gf_log(GF_LOG_ERROR, "recipe: dependency %s: spec must be a string",
                   key);
            goto fail;
        }
        /* a spec is either a version constraint ("*", "^1.2", ">=1,<2")
         * or a qualified package reference ("github:owner/repo@1.2.0",
         * "local:/path", "https://host/owner/repo") — the resolver
         * understands both forms. */
        bool is_ref = strstr(spec, "://") || strchr(spec, '/') ||
                      gf_str_starts_with(spec, "local:") ||
                      strchr(spec, ':');
        gf_vcset check;
        if (!is_ref && !gf_vcset_parse(spec, &check)) {
            gf_log(GF_LOG_ERROR, "recipe: dependency %s: bad version spec: %s",
                   key, spec);
            gf_vcset_free(&check);
            goto fail;
        }
        if (!is_ref)
            gf_vcset_free(&check);
        nm[*n] = gf_strdup(key);
        sp[*n] = gf_strdup(spec);
        (*n)++;
    }
    *names = nm;
    *specs = sp;
    return 0;
fail:
    for (size_t i = 0; i < *n; i++) {
        free(nm[i]);
        free(sp[i]);
    }
    free(nm);
    free(sp);
    return -1;
}

static bool valid_build_system(const char *s)
{
    static const char *ok[] = { "cmake", "meson",  "make",  "autotools",
                                "cargo", "go",     "python", "node",
                                "vala",  "custom", NULL };
    for (int i = 0; ok[i]; i++)
        if (gf_str_ieq(s, ok[i]))
            return true;
    return false;
}

gf_recipe *gf_recipe_load(const char *source_dir)
{
    gf_recipe *r = gf_calloc(1, sizeof(gf_recipe));
    r->jobs = -1;
    char *path = gf_path_join(source_dir, "gitfull.toml");
    if (!gf_fs_is_file(path)) {
        free(path);
        return r;
    }
    size_t len = 0;
    char *text = gf_fs_read_file_limit(path, 1 << 20, &len);
    if (!text) {
        free(path);
        gf_recipe_free(r);
        return NULL;
    }
    char err[256];
    gf_json *root = gf_toml_parse(text, len, err, sizeof(err));
    free(text);
    if (!root) {
        gf_log(GF_LOG_ERROR, "%s: %s", path, err);
        free(path);
        gf_recipe_free(r);
        return NULL;
    }
    r->present = true;
    r->path = path;

    /* [package] */
    const gf_json *pkg = gf_json_get(root, "package");
    if (pkg) {
        const char *v = gf_json_str(gf_json_get(pkg, "name"));
        if (v && !gf_path_component_ok(v)) {
            gf_log(GF_LOG_ERROR, "recipe: [package] name must be a single "
                                 "safe component");
            goto fail;
        }
        r->name = v ? gf_strdup(v) : NULL;
        v = gf_json_str(gf_json_get(pkg, "version"));
        r->version = v ? gf_strdup(v) : NULL;
        v = gf_json_str(gf_json_get(pkg, "description"));
        r->description = v ? gf_strdup(v) : NULL;
        v = gf_json_str(gf_json_get(pkg, "license"));
        r->license = v ? gf_strdup(v) : NULL;
        v = gf_json_str(gf_json_get(pkg, "homepage"));
        r->homepage = v ? gf_strdup(v) : NULL;
    }

    /* [source] */
    const gf_json *src = gf_json_get(root, "source");
    if (src) {
        const char *v = gf_json_str(gf_json_get(src, "subdir"));
        if (v && (!gf_path_component_ok(v))) {
            gf_log(GF_LOG_ERROR, "recipe: [source] subdir must be a single "
                                 "directory component");
            goto fail;
        }
        r->subdir = v ? gf_strdup(v) : NULL;
    }

    /* [build] */
    const gf_json *build = gf_json_get(root, "build");
    if (build) {
        const char *v = gf_json_str(gf_json_get(build, "system"));
        if (v && !valid_build_system(v)) {
            gf_log(GF_LOG_ERROR, "recipe: [build] system: unknown '%s'", v);
            goto fail;
        }
        r->build_system = v ? gf_strdup(v) : NULL;
        double j = gf_json_num(gf_json_get(build, "jobs"), -1);
        if (j >= 1 && j <= 1024)
            r->jobs = (int)j;
        const gf_json *rt = gf_json_get(build, "run_tests");
        if (rt && rt->type == GF_JSON_BOOL) {
            r->run_tests = rt->v.b;
            r->run_tests_set = true;
        }
    }
    /* [build.env] */
    {
        const gf_json *build2 = gf_json_get(root, "build");
        const gf_json *env = build2 ? gf_json_get(build2, "env") : NULL;
        if (env && env->type == GF_JSON_OBJECT) {
            size_t n = gf_json_len(env);
            r->env = gf_malloc((n + 1) * sizeof(char *));
            for (size_t i = 0; i < n; i++) {
                const char *k = gf_json_key(env, i);
                const char *v = gf_json_str(gf_json_val(env, i));
                if (!v) {
                    gf_log(GF_LOG_ERROR, "recipe: [build.env] %s: value must "
                                         "be a string", k);
                    goto fail;
                }
                char *kv = gf_malloc(strlen(k) + strlen(v) + 2);
                sprintf(kv, "%s=%s", k, v);
                r->env[r->env_count++] = kv;
            }
            r->env[n] = NULL;
        }
    }
    /* [[build.steps]] etc. */
    {
        const gf_json *build3 = gf_json_get(root, "build");
        const gf_json *st = build3 ? gf_json_get(build3, "steps") : NULL;
        const gf_json *ist = build3 ? gf_json_get(build3, "install_steps") : NULL;
        const gf_json *tst = build3 ? gf_json_get(build3, "test_steps") : NULL;
        if (steps_from_json(st, &r->steps, &r->nsteps) != 0)
            goto fail;
        if (steps_from_json(ist, &r->install_steps, &r->ninstall) != 0)
            goto fail;
        if (steps_from_json(tst, &r->test_steps, &r->ntest) != 0)
            goto fail;
    }

    /* dependencies */
    if (deps_from_json(gf_json_get(root, "dependencies"), &r->dep_names,
                       &r->dep_specs, &r->ndeps) != 0)
        goto fail;
    if (deps_from_json(gf_json_get(root, "build-dependencies"), &r->bdep_names,
                       &r->bdep_specs, &r->nbdeps) != 0)
        goto fail;
    if (deps_from_json(gf_json_get(root, "optional-dependencies"),
                       &r->odep_names, &r->odep_specs, &r->nodeps) != 0)
        goto fail;
    if (deps_from_json(gf_json_get(root, "test-dependencies"),
                       &r->tdep_names, &r->tdep_specs, &r->ntdeps) != 0)
        goto fail;

    /* [package] host constraints */
    {
        const gf_json *pkgj = gf_json_get(root, "package");
        const char *v = pkgj ? gf_json_str(gf_json_get(pkgj, "arch")) : NULL;
        r->arch = v ? gf_strdup(v) : NULL;
        v = pkgj ? gf_json_str(gf_json_get(pkgj, "abi")) : NULL;
        r->abi = v ? gf_strdup(v) : NULL;
    }

    /* [release] */
    const gf_json *rel = gf_json_get(root, "release");
    if (rel) {
        const char *v = gf_json_str(gf_json_get(rel, "pin_tag"));
        r->pin_tag = v ? gf_strdup(v) : NULL;
        v = gf_json_str(gf_json_get(rel, "channel"));
        if (v && strcmp(v, "stable") != 0 && strcmp(v, "prerelease") != 0 &&
            strcmp(v, "edge") != 0) {
            gf_log(GF_LOG_ERROR,
                   "recipe: [release] channel must be stable|prerelease|edge");
            goto fail;
        }
        r->channel = v ? gf_strdup(v) : NULL;
    }

    gf_json_free(root);
    return r;

fail:
    gf_json_free(root);
    gf_recipe_free(r);
    return NULL;
}

void gf_recipe_free(gf_recipe *r)
{
    if (!r)
        return;
    free(r->path);
    free(r->name);
    free(r->version);
    free(r->description);
    free(r->license);
    free(r->homepage);
    free(r->subdir);
    free(r->build_system);
    if (r->env) {
        for (size_t i = 0; r->env[i]; i++)
            free(r->env[i]);
        free(r->env);
    }
    for (size_t i = 0; i < r->nsteps; i++) {
        for (size_t k = 0; r->steps[i].argv && r->steps[i].argv[k]; k++)
            free(r->steps[i].argv[k]);
        free(r->steps[i].argv);
        free(r->steps[i].cwd);
        free(r->steps[i].desc);
    }
    free(r->steps);
    for (size_t i = 0; i < r->ninstall; i++) {
        for (size_t k = 0; r->install_steps[i].argv && r->install_steps[i].argv[k]; k++)
            free(r->install_steps[i].argv[k]);
        free(r->install_steps[i].argv);
        free(r->install_steps[i].cwd);
        free(r->install_steps[i].desc);
    }
    free(r->install_steps);
    for (size_t i = 0; i < r->ntest; i++) {
        for (size_t k = 0; r->test_steps[i].argv && r->test_steps[i].argv[k]; k++)
            free(r->test_steps[i].argv[k]);
        free(r->test_steps[i].argv);
        free(r->test_steps[i].cwd);
        free(r->test_steps[i].desc);
    }
    free(r->test_steps);
    for (size_t i = 0; i < r->ndeps; i++) {
        free(r->dep_names[i]);
        free(r->dep_specs[i]);
    }
    free(r->dep_names);
    free(r->dep_specs);
    for (size_t i = 0; i < r->nbdeps; i++) {
        free(r->bdep_names[i]);
        free(r->bdep_specs[i]);
    }
    free(r->bdep_names);
    free(r->bdep_specs);
    for (size_t i = 0; i < r->nodeps; i++) {
        free(r->odep_names[i]);
        free(r->odep_specs[i]);
    }
    free(r->odep_names);
    free(r->odep_specs);
    for (size_t i = 0; i < r->ntdeps; i++) {
        free(r->tdep_names[i]);
        free(r->tdep_specs[i]);
    }
    free(r->tdep_names);
    free(r->tdep_specs);
    free(r->arch);
    free(r->abi);
    free(r->pin_tag);
    free(r->channel);
    free(r);
}
