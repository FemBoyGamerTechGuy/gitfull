/* recipe.h — optional gitfull.toml recipe support.
 *
 * Recipes are an OVERRIDE mechanism; autodetection remains the default.
 * Simple projects must not need a recipe.
 */
#ifndef GF_RECIPE_H
#define GF_RECIPE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_recipe_step {
    char **argv;      /* NULL-terminated argv array (owned) */
    char *cwd;        /* NULL = build dir; "src" = source dir; relative path */
    char *desc;       /* human label or NULL */
} gf_recipe_step;

typedef struct gf_recipe {
    bool present;
    char *path;              /* gitfull.toml path */
    /* [package] */
    char *name;              /* override package name */
    char *version;           /* override version (rare, recorded) */
    char *description;
    char *license;
    char *homepage;
    /* [source] */
    char *subdir;            /* source subdirectory when repo root != source */
    /* [build] */
    char *build_system;      /* NULL = autodetect; else cmake|meson|make|... */
    int jobs;                /* -1 = not set */
    bool run_tests;          /* absent = use global config */
    bool run_tests_set;
    /* [build.env] extra environment K=V */
    char **env;              /* NULL-terminated, NULL when absent */
    size_t env_count;
    /* [[build.steps]] / [[build.install_steps]] / [[build.test_steps]] */
    gf_recipe_step *steps;
    size_t nsteps;
    gf_recipe_step *install_steps;
    size_t ninstall;
    gf_recipe_step *test_steps;
    size_t ntest;
    /* [dependencies] / [build-dependencies] / [optional-dependencies] /
     * [test-dependencies]: name -> spec */
    char **dep_names;
    char **dep_specs;
    size_t ndeps;
    char **bdep_names;
    char **bdep_specs;
    size_t nbdeps;
    char **odep_names;
    char **odep_specs;
    size_t nodeps;
    char **tdep_names;
    char **tdep_specs;
    size_t ntdeps;
    /* [package] host constraints (optional; NULL = any) */
    char *arch;               /* e.g. "x86_64"; must match config arch */
    char *abi;                /* free-form ABI tag; pairwise conflicts detected */
    /* [release] */
    char *pin_tag;           /* exact tag hint */
    char *channel;           /* stable|prerelease|edge */
} gf_recipe;

/* Load gitfull.toml from source_dir (or source_dir/subdir). Returns a recipe
 * (present=false) when no recipe exists. NULL only on hard errors (bad TOML
 * syntax, malformed recipe fields). */
gf_recipe *gf_recipe_load(const char *source_dir);
void gf_recipe_free(gf_recipe *r);

#endif /* GF_RECIPE_H */
