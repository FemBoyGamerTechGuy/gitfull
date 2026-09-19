/* toolchain.c — toolchain identity recording and bootstrap probing. */
#include "toolchain.h"

#include "common.h"
#include "detect.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void tool_free(gf_tool *t)
{
    free(t->name);
    free(t->path);
    free(t->source);
    free(t->version);
    free(t->sha256);
}

void gf_toolchain_free(gf_toolchain *tc)
{
    if (!tc)
        return;
    for (size_t i = 0; i < tc->ntools; i++)
        tool_free(&tc->tools[i]);
    free(tc->tools);
    free(tc->mode);
    free(tc->id);
    free(tc->dir);
    for (size_t i = 0; i < tc->nsystem_dirs; i++)
        free(tc->system_dirs[i]);
    free(tc->system_dirs);
    memset(tc, 0, sizeof(*tc));
}

const gf_tool *gf_toolchain_get(const gf_toolchain *tc, const char *name)
{
    if (!tc)
        return NULL;
    for (size_t i = 0; i < tc->ntools; i++) {
        if (strcmp(tc->tools[i].name, name) == 0)
            return &tc->tools[i];
    }
    return NULL;
}

/* which() across a fixed set of standard prefixes (no PATH trust: PATH is
 * caller-controlled; we probe well-known absolute locations only). */
static char *find_tool(const char *name)
{
    static const char *prefixes[] = {
        "/usr/local/bin", "/usr/bin", "/bin", "/usr/local/sbin",
        "/usr/sbin", "/sbin", NULL
    };
    for (int i = 0; prefixes[i]; i++) {
        char *p = gf_path_join(prefixes[i], name);
        /* only accept regular files (or symlinks to them) */
        struct stat st;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            return p;
        }
        free(p);
    }
    return NULL;
}

/* Capture first line of `tool --version` output. */
static char *tool_version(const char *path)
{
    const char *argv[] = { path, "--version", NULL };
    gf_strbuf out;
    gf_strbuf_init(&out);
    gf_exec_opts eo = gf_exec_opts_default();
    eo.out = &out;
    gf_exec_result res;
    memset(&res, 0, sizeof(res));
    eo.timeout_ms = 20000;
    if (gf_exec_capture(argv, &eo, &res) != 0 || res.status != 0) {
        gf_strbuf_free(&out);
        return NULL;
    }
    char *v = gf_strbuf_steal(&out);
    char *nl = strchr(v, '\n');
    if (nl)
        *nl = '\0';
    gf_str_trim(v);
    return v;
}

static void add_tool(gf_toolchain *tc, const char *name, const char *host_path)
{
    /* dedupe by name */
    if (gf_toolchain_get(tc, name))
        return;
    gf_tool t = { 0 };
    t.name = gf_strdup(name);
    t.source = gf_strdup(host_path);
    /* sandbox-visible path: keep the host absolute path (system dirs are
     * bind-mounted at identical paths inside the sandbox) */
    t.path = gf_strdup(host_path);
    t.version = tool_version(host_path);
    t.sha256 = gf_sha256_file_hex(host_path);
    tc->tools = gf_realloc(tc->tools, (tc->ntools + 1) * sizeof(gf_tool));
    tc->tools[tc->ntools++] = t;
}

/* alias registrations: e.g. "cc" also probes gcc/clang */
static void probe_tool(gf_toolchain *tc, const char *name)
{
    static const char *cc_alt[] = { "cc", "gcc", "clang", NULL };
    static const char *cxx_alt[] = { "c++", "g++", "clang++", NULL };
    static const char *sh_alt[] = { "sh", "dash", "bash", "bash5", NULL };
    const char **alts = NULL;
    if (strcmp(name, "cc") == 0)
        alts = cc_alt;
    else if (strcmp(name, "c++") == 0)
        alts = cxx_alt;
    else if (strcmp(name, "sh") == 0)
        alts = sh_alt;
    if (alts) {
        for (int i = 0; alts[i]; i++) {
            char *p = find_tool(alts[i]);
            if (p) {
                add_tool(tc, name, p);
                free(p);
                return;
            }
        }
        return;
    }
    char *p = find_tool(name);
    if (p) {
        add_tool(tc, name, p);
        free(p);
    }
}

int gf_toolchain_probe(const gf_config *cfg, const char *build_system,
                       gf_toolchain *tc)
{
    memset(tc, 0, sizeof(*tc));
    tc->mode = gf_strdup(cfg->toolchain_mode ? cfg->toolchain_mode
                                             : "bootstrap");

    /* base tools always recorded when present */
    static const char *base[] = { "sh", "cc", "c++", "make", "ar", "ld", NULL };
    for (int i = 0; base[i]; i++)
        probe_tool(tc, base[i]);

    /* build-system specific tools */
    gf_bsys b = GF_BSYS_NONE;
    for (int i = 0; ; i++) {
        const char *n = gf_bsys_name((gf_bsys)i);
        if (!n)
            break;
        if (build_system && gf_str_ieq(n, build_system)) {
            b = (gf_bsys)i;
            break;
        }
    }
    if (b == GF_BSYS_NONE && build_system) {
        /* names like "custom" or unknown: accept as-is */
    }
    if (b != GF_BSYS_NONE) {
        const char *const *tools = gf_bsys_tools(b);
        for (int i = 0; tools[i]; i++)
            probe_tool(tc, tools[i]);
    }

    if (gf_str_ieq(tc->mode, "pinned")) {
        /* pinned mode: use <state>/toolchains/pinned when fully populated;
         * otherwise fall back to bootstrap with a warning (honest boundary) */
        char *pin_dir = gf_path_join_multi(cfg->state_dir, "toolchains",
                                           "pinned", NULL);
        if (gf_fs_is_dir(pin_dir) && gf_toolchain_get(tc, "cc") &&
            gf_str_starts_with(gf_toolchain_get(tc, "cc")->source, pin_dir)) {
            tc->dir = pin_dir;
        } else {
            gf_log(GF_LOG_WARN,
                   "toolchain: pinned mode requested but no pinned toolchain "
                   "is populated; falling back to bootstrap (recorded)");
            free(tc->mode);
            tc->mode = gf_strdup("bootstrap");
            free(pin_dir);
        }
    }

    /* bootstrap: bind these host dirs read-only in the sandbox
     * (NULL-terminated array; must outlive the sandbox runs) */
    static const char *dirs[] = { "/usr", "/lib", "/lib64", "/bin", "/sbin",
                                  NULL };
    tc->system_dirs = gf_malloc(8 * sizeof(char *));
    memset(tc->system_dirs, 0, 8 * sizeof(char *));
    for (int i = 0; dirs[i]; i++) {
        if (!gf_fs_is_dir(dirs[i]))
            continue;
        tc->system_dirs[tc->nsystem_dirs++] = gf_strdup(dirs[i]);
    }

    /* toolchain dir: holds the manifest at /toolchain inside the sandbox */
    tc->dir = gf_path_join_multi(cfg->state_dir, "toolchains", "bootstrap",
                                 NULL);
    if (gf_fs_mkdir_p(tc->dir) != 0) {
        gf_toolchain_free(tc);
        return -1;
    }
    gf_json *man = gf_toolchain_manifest(tc);
    if (man) {
        char *dump = gf_json_dump_pretty(man);
        char *mp = gf_path_join(tc->dir, "manifest.json");
        gf_fs_write_file_atomic(mp, dump, strlen(dump));
        free(mp);
        free(dump);
        gf_json_free(man);
    }
    return 0;
}

static int tool_cmp(const void *a, const void *b)
{
    const gf_tool *ta = a, *tb = b;
    return strcmp(ta->name, tb->name);
}

gf_json *gf_toolchain_manifest(const gf_toolchain *tc)
{
    gf_json *root = gf_json_new_object();
    gf_json_object_set(root, "mode", gf_json_new_string(tc->mode));
    gf_json *tools = gf_json_new_array();
    gf_tool *sorted = gf_memdup(tc->tools, tc->ntools * sizeof(gf_tool));
    if (sorted && tc->ntools > 1)
        qsort(sorted, tc->ntools, sizeof(gf_tool), tool_cmp);
    for (size_t i = 0; i < tc->ntools; i++) {
        const gf_tool *t = &sorted[i];
        gf_json *o = gf_json_new_object();
        gf_json_object_set(o, "name", gf_json_new_string(t->name));
        gf_json_object_set(o, "path", gf_json_new_string(t->path));
        if (t->source)
            gf_json_object_set(o, "source", gf_json_new_string(t->source));
        if (t->version)
            gf_json_object_set(o, "version", gf_json_new_string(t->version));
        if (t->sha256)
            gf_json_object_set(o, "sha256", gf_json_new_string(t->sha256));
        gf_json_array_push(tools, o);
    }
    free(sorted);
    gf_json_object_set(root, "tools", tools);
    gf_json *sysd = gf_json_new_array();
    for (size_t i = 0; i < tc->nsystem_dirs; i++)
        gf_json_array_push(sysd, gf_json_new_string(tc->system_dirs[i]));
    gf_json_object_set(root, "system_dirs", sysd);
    return root;
}
