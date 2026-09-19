/* config.c — configuration loading, defaults, and repo management. */
#include "config.h"

#include "common.h"
#include "ini.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define GF_DEFAULT_STATE "/var/lib/gitfull"
#define GF_DEFAULT_PREFIX "/usr/local"
#define GF_DEFAULT_CONF "/etc/gitfull.conf"

const char *gf_config_default_text(void)
{
    return
        "# gitfull configuration\n"
        "# See docs/configuration.md for all options.\n"
        "\n"
        "[general]\n"
        "# default repository used for unqualified owner/repo references\n"
        "default_repository=github\n"
        "\n"
        "[build]\n"
        "# installation prefix for packages\n"
        "prefix=/usr/local\n"
        "# parallel build jobs (0 = auto)\n"
        "jobs=0\n"
        "# run package test suites during builds\n"
        "run_tests=true\n"
        "# toolchain identity mode: bootstrap (host tools, recorded) or pinned\n"
        "toolchain_mode=bootstrap\n"
        "# cgroup v2 resource limits for builds (0 = disabled; requires a\n"
        "# delegated unified hierarchy — skipped silently otherwise)\n"
        "memory_limit_mb=0\n"
        "cpu_quota_percent=0\n"
        "\n"
        "[sandbox]\n"
        "# isolation level for builds: userns | chroot | none\n"
        "level=userns\n"
        "# build-time network access: none | fetch\n"
        "network=none\n"
        "# seccomp syscall filter (allowlist + privilege/escape denylist)\n"
        "seccomp=true\n"
        "\n"
        "[rollback]\n"
        "# previous versions retained for rollback per package\n"
        "hold=3\n"
        "\n"
        "[hold]\n"
        "# packages held back from automatic upgrades (comma separated)\n"
        "packages=\n"
        "\n"
        "[network]\n"
        "timeout_secs=120\n"
        "max_download_mb=2048\n"
        "retries=2\n"
        "\n"
        "[repositories.github]\n"
        "enabled=true\n"
        "kind=github\n"
        "api_url=https://api.github.com\n"
        "# recommended: export GITFULL_GITHUB_TOKEN (read at runtime, never stored)\n"
        "token_env=GITFULL_GITHUB_TOKEN\n"
        "\n"
        "[repositories.codeberg]\n"
        "enabled=false\n"
        "kind=gitea\n"
        "api_url=https://codeberg.org/api/v1\n"
        "\n"
        "[repositories.gitlab]\n"
        "enabled=false\n"
        "kind=gitlab\n"
        "api_url=https://gitlab.com/api/v4\n"
        "\n";
}

const char *gf_config_default_arch(void)
{
    static char arch[128];
    static bool done = false;
    if (!done) {
        struct utsname u;
        if (uname(&u) == 0)
            snprintf(arch, sizeof(arch), "%s", u.machine);
        else
            snprintf(arch, sizeof(arch), "unknown");
        done = true;
    }
    return arch;
}

static int cfg_int(const gf_ini *ini, const char *sec, const char *key,
                   int def, int min, int max)
{
    const char *v = gf_ini_get(ini, sec, key);
    if (!v || !*v)
        return def;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (!end || *end != '\0' || errno != 0) {
        gf_log(GF_LOG_WARN, "config: [%s] %s: not a number (%s), using %d",
               sec, key, v, def);
        return def;
    }
    if (n < min || n > max) {
        gf_log(GF_LOG_WARN, "config: [%s] %s: out of range (%s), using %d",
               sec, key, v, def);
        return def;
    }
    return (int)n;
}

static char *derive_web_base(const char *kind, const char *api_url)
{
    if (!api_url)
        return NULL;
    if (gf_str_ieq(kind, "github")) {
        /* api.github.com -> github.com; api.host -> host when self-hosted */
        if (strcmp(api_url, "https://api.github.com") == 0)
            return gf_strdup("https://github.com");
    }
    if (gf_str_ieq(kind, "gitlab")) {
        /* strip trailing /api/v4 or /api/v3 */
        char *copy = gf_strdup(api_url);
        for (;;) {
            if (gf_str_ends_with(copy, "/api/v4"))
                copy[strlen(copy) - 7] = '\0';
            else if (gf_str_ends_with(copy, "/api/v3"))
                copy[strlen(copy) - 7] = '\0';
            else if (gf_str_ends_with(copy, "/api"))
                copy[strlen(copy) - 4] = '\0';
            else
                break;
        }
        return copy;
    }
    if (gf_str_ieq(kind, "gitea")) {
        char *copy = gf_strdup(api_url);
        if (gf_str_ends_with(copy, "/api/v1"))
            copy[strlen(copy) - 6] = '\0';
        return copy;
    }
    /* generic: web base is the url itself */
    return gf_strdup(api_url);
}

static void repo_ensure_cap(gf_config *cfg, size_t need)
{
    if (cfg->nrepos_cap >= need)
        return;
    size_t ncap = cfg->nrepos_cap ? cfg->nrepos_cap * 2 : 16;
    while (ncap < need)
        ncap *= 2;
    gf_repo *grown = gf_calloc(ncap, sizeof(gf_repo));
    if (cfg->repos && cfg->nrepos)
        memcpy(grown, cfg->repos, cfg->nrepos * sizeof(gf_repo));
    free(cfg->repos);
    cfg->repos = grown;
    cfg->nrepos_cap = ncap;
}

static void add_repo(gf_config *cfg, const char *name, const gf_ini *ini)
{
    char sec[256];
    snprintf(sec, sizeof(sec), "repositories.%s", name);
    const char *kind = gf_ini_get_def(ini, sec, "kind", "generic");
    const char *api_url = gf_ini_get(ini, sec, "api_url");
    if (!api_url)
        api_url = gf_ini_get(ini, sec, "url");
    if (!api_url || !*api_url) {
        gf_log(GF_LOG_WARN,
               "config: [repositories.%s]: missing api_url/url, ignoring",
               name);
        return;
    }
    if (!gf_str_starts_with(api_url, "https://") &&
        !gf_ini_get_bool(ini, sec, "allow_http", false)) {
        gf_log(GF_LOG_WARN,
               "config: [repositories.%s]: api_url is not https and "
               "allow_http is not set, ignoring (refusing plaintext)",
               name);
        return;
    }
    gf_repo *r = &cfg->repos[cfg->nrepos];
    memset(r, 0, sizeof(*r));
    r->name = gf_strdup(name);
    r->kind = gf_strdup(kind);
    r->api_url = gf_strdup(api_url);
    r->web_url = derive_web_base(kind, api_url);
    r->enabled = gf_ini_get_bool(ini, sec, "enabled", false);
    r->allow_http = gf_ini_get_bool(ini, sec, "allow_http", false);
    r->token_file = gf_strdup(gf_ini_get(ini, sec, "token_file") ? gf_ini_get(ini, sec, "token_file") : "");
    r->token_env = gf_strdup(gf_ini_get(ini, sec, "token_env") ? gf_ini_get(ini, sec, "token_env") : "");
    if (r->token_file && !*r->token_file) { free(r->token_file); r->token_file = NULL; }
    if (r->token_env && !*r->token_env) { free(r->token_env); r->token_env = NULL; }
    cfg->nrepos++;
}

gf_config *gf_config_load(const char *root, const char *config_path)
{
    gf_config *cfg = gf_calloc(1, sizeof(gf_config));
    if (root && *root) {
        /* normalize: strip trailing slashes */
        cfg->root = gf_path_normalize(root);
    } else {
        cfg->root = gf_strdup("");
    }
    cfg->arch = gf_strdup(gf_config_default_arch());
    cfg->default_repo = gf_strdup("github");
    cfg->toolchain_mode = gf_strdup("bootstrap");
    cfg->sandbox_level = gf_strdup("userns");
    cfg->sandbox_network = gf_strdup("none");
    cfg->sandbox_seccomp = true;
    cfg->memory_limit_mb = 0;
    cfg->cpu_quota_percent = 0;
    cfg->log_level = gf_strdup("info");
    cfg->escalation = gf_strdup("auto");
    cfg->rollback_hold = 3;
    cfg->jobs = 0;
    cfg->timeout_secs = 120;
    cfg->max_download_mb = 2048;
    cfg->retries = 2;
    cfg->run_tests = true;
    cfg->interactive = true;
    cfg->repos = gf_calloc(16, sizeof(gf_repo));
    cfg->nrepos = 0;
    cfg->nrepos_cap = 16;

    /* default built-in repositories (matching default config text) */
    {
        gf_repo *r = &cfg->repos[cfg->nrepos++];
        r->name = gf_strdup("github");
        r->kind = gf_strdup("github");
        r->api_url = gf_strdup("https://api.github.com");
        r->web_url = gf_strdup("https://github.com");
        r->enabled = true;
        r->token_env = gf_strdup("GITFULL_GITHUB_TOKEN");
        r = &cfg->repos[cfg->nrepos++];
        r->name = gf_strdup("codeberg");
        r->kind = gf_strdup("gitea");
        r->api_url = gf_strdup("https://codeberg.org/api/v1");
        r->web_url = gf_strdup("https://codeberg.org");
        r->enabled = false;
        r = &cfg->repos[cfg->nrepos++];
        r->name = gf_strdup("gitlab");
        r->kind = gf_strdup("gitlab");
        r->api_url = gf_strdup("https://gitlab.com/api/v4");
        r->web_url = gf_strdup("https://gitlab.com");
        r->enabled = false;
    }

    /* path resolution */
    char *env_conf = getenv("GITFULL_CONFIG");
    const char *use_path = NULL;
    if (config_path && *config_path)
        use_path = config_path;
    else if (env_conf && *env_conf)
        use_path = env_conf;
    else {
        char *cand;
        if (*cfg->root) {
            cand = gf_path_join(cfg->root, GF_DEFAULT_CONF);
        } else {
            cand = gf_strdup(GF_DEFAULT_CONF);
        }
        if (gf_fs_is_file(cand))
            use_path = cand;
        else {
            free(cand);
            cand = gf_strdup(GF_DEFAULT_CONF);
            if (gf_fs_is_file(cand))
                use_path = cand;
            else {
                free(cand);
                cand = NULL;
            }
        }
        free(cand);
        cand = NULL;
    }

    if (use_path) {
        char err[256];
        gf_ini *ini = gf_ini_parse_file(use_path, err, sizeof(err));
        if (!ini) {
            gf_log(GF_LOG_ERROR, "config: %s: %s", use_path, err);
            gf_config_free(cfg);
            return NULL;
        }
        cfg->path = gf_strdup(use_path);

        /* [general] */
        const char *v = gf_ini_get(ini, "general", "default_repository");
        if (v && *v) {
            free(cfg->default_repo);
            cfg->default_repo = gf_strdup(v);
        }
        v = gf_ini_get(ini, "general", "log_level");
        if (v && *v) {
            free(cfg->log_level);
            cfg->log_level = gf_strdup(v);
        }

        /* [build] */
        v = gf_ini_get(ini, "build", "prefix");
        if (v && *v) {
            char *norm = gf_path_normalize(*cfg->root && !gf_path_is_abs(v)
                                               ? gf_path_join(cfg->root, v)
                                               : v);
            cfg->prefix = norm;
        }
        cfg->jobs = cfg_int(ini, "build", "jobs", cfg->jobs, 0, 1024);
        cfg->run_tests = gf_ini_get_bool(ini, "build", "run_tests", cfg->run_tests);
        v = gf_ini_get(ini, "build", "toolchain_mode");
        if (v && *v) {
            if (!gf_str_ieq(v, "bootstrap") && !gf_str_ieq(v, "pinned")) {
                gf_log(GF_LOG_WARN,
                       "config: [build] toolchain_mode: unknown '%s' "
                       "(use bootstrap|pinned)", v);
            } else {
                free(cfg->toolchain_mode);
                cfg->toolchain_mode = gf_strdup(v);
            }
        }

        /* [sandbox] */
        v = gf_ini_get(ini, "sandbox", "level");
        if (v && *v) {
            free(cfg->sandbox_level);
            cfg->sandbox_level = gf_strdup(v);
        }
        v = gf_ini_get(ini, "sandbox", "network");
        if (v && *v) {
            free(cfg->sandbox_network);
            cfg->sandbox_network = gf_strdup(v);
        }
        cfg->sandbox_seccomp = gf_ini_get_bool(ini, "sandbox", "seccomp",
                                               cfg->sandbox_seccomp);
        cfg->memory_limit_mb = cfg_int(ini, "build", "memory_limit_mb",
                                       cfg->memory_limit_mb, 0, 1 << 20);
        cfg->cpu_quota_percent = cfg_int(ini, "build", "cpu_quota_percent",
                                         cfg->cpu_quota_percent, 0, 10000);

        /* [rollback] */
        cfg->rollback_hold = cfg_int(ini, "rollback", "hold",
                                     cfg->rollback_hold, 0, 64);

        /* [network] */
        cfg->timeout_secs = cfg_int(ini, "network", "timeout_secs",
                                    cfg->timeout_secs, 1, 3600);
        cfg->max_download_mb = cfg_int(ini, "network", "max_download_mb",
                                       cfg->max_download_mb, 1, 1 << 20);
        cfg->retries = cfg_int(ini, "network", "retries", cfg->retries, 0, 10);

        /* [hold] */
        v = gf_ini_get(ini, "hold", "packages");
        if (v && *v) {
            cfg->holds = gf_str_split(v, ",", &cfg->nholds);
            for (size_t i = 0; i < cfg->nholds; i++)
                gf_str_trim(cfg->holds[i]);
        }

        /* [privilege] */
        v = gf_ini_get(ini, "privilege", "escalation");
        if (v && *v) {
            free(cfg->escalation);
            cfg->escalation = gf_strdup(v);
        }

        /* [repositories.*] — overrides/extends built-ins */
        for (size_t i = 0; i < gf_ini_section_count(ini); i++) {
            const char *sec = gf_ini_section(ini, i);
            if (!gf_str_starts_with(sec, "repositories."))
                continue;
            const char *name = sec + strlen("repositories.");
            if (!*name)
                continue;
            char kind[32];
            snprintf(kind, sizeof(kind), "%s",
                     gf_ini_get_def(ini, sec, "kind", "generic"));
            /* find existing repo with same name */
            gf_repo *found = NULL;
            for (size_t k = 0; k < cfg->nrepos; k++) {
                if (strcmp(cfg->repos[k].name, name) == 0) {
                    found = &cfg->repos[k];
                    break;
                }
            }
            if (found) {
                /* merge into existing slot */
                const char *api = gf_ini_get(ini, sec, "api_url");
                if (!api)
                    api = gf_ini_get(ini, sec, "url");
                free(found->kind);
                found->kind = gf_strdup(kind);
                if (api && *api) {
                    free(found->api_url);
                    found->api_url = gf_strdup(api);
                    free(found->web_url);
                    found->web_url = derive_web_base(kind, api);
                }
                found->enabled = gf_ini_get_bool(ini, sec, "enabled",
                                                 found->enabled);
                found->allow_http = gf_ini_get_bool(ini, sec, "allow_http",
                                                    false);
                const char *tf = gf_ini_get(ini, sec, "token_file");
                free(found->token_file);
                found->token_file = tf && *tf ? gf_strdup(tf) : NULL;
                const char *te = gf_ini_get(ini, sec, "token_env");
                free(found->token_env);
                found->token_env = te && *te ? gf_strdup(te) : NULL;
                continue;
            }
            repo_ensure_cap(cfg, cfg->nrepos + 1);
            char *name_copy = gf_strdup(name);
            add_repo(cfg, name_copy, ini);
            free(name_copy);
        }
        gf_ini_free(ini);
    }

    /* fallback defaults for derived paths */
    if (!cfg->prefix) {
        cfg->prefix = *cfg->root
            ? gf_path_join(cfg->root, GF_DEFAULT_PREFIX)
            : gf_strdup(GF_DEFAULT_PREFIX);
    }
    {
        const char *env_state = getenv("GITFULL_STATE_DIR");
        if (env_state && *env_state)
            cfg->state_dir = gf_strdup(env_state);
        else
            cfg->state_dir = *cfg->root
                ? gf_path_join(cfg->root, GF_DEFAULT_STATE)
                : gf_strdup(GF_DEFAULT_STATE);
    }
    cfg->etc_conf = *cfg->root
        ? gf_path_join(cfg->root, GF_DEFAULT_CONF)
        : gf_strdup(GF_DEFAULT_CONF);

    if (cfg->jobs <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cfg->jobs = n > 0 ? (int)(n > 64 ? 64 : n) : 2;
    }
    return cfg;
}

int gf_config_validate_runtime(const gf_config *cfg)
{
    static const char *keys[4] = { "prefix", "root", "state_dir", NULL };
    const char *vals[3];
    vals[0] = cfg->prefix;
    vals[1] = cfg->root;
    vals[2] = cfg->state_dir;
    for (int i = 0; keys[i] && i < 3; i++) {
        const char *v = vals[i];
        if (!v || !*v)
            continue;
        for (const char *p = v; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '/')
                continue;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                c == '-' || c == '+')
                continue;
            gf_log(GF_LOG_ERROR,
                   "config: %s '%s' contains unsafe character '%c' "
                   "(allowed: letters, digits, '.', '_', '-', '+' and '/'); "
                   "refusing — build environments interpolate these values",
                   keys[i], v, (char)c);
            return -1;
        }
    }
    return 0;
}

void gf_config_free(gf_config *cfg)
{
    if (!cfg)
        return;
    free(cfg->path);
    free(cfg->root);
    free(cfg->arch);
    free(cfg->state_dir);
    free(cfg->prefix);
    free(cfg->etc_conf);
    free(cfg->default_repo);
    free(cfg->toolchain_mode);
    free(cfg->sandbox_level);
    free(cfg->sandbox_network);
    free(cfg->log_level);
    free(cfg->escalation);
    for (size_t i = 0; i < cfg->nholds; i++)
        free(cfg->holds[i]);
    free(cfg->holds);
    for (size_t i = 0; i < cfg->nrepos; i++) {
        free(cfg->repos[i].name);
        free(cfg->repos[i].kind);
        free(cfg->repos[i].api_url);
        free(cfg->repos[i].web_url);
        free(cfg->repos[i].token_file);
        free(cfg->repos[i].token_env);
    }
    free(cfg->repos);
    free(cfg);
}

const gf_repo *gf_config_repo(const gf_config *cfg, const char *name)
{
    if (!cfg || !name)
        return NULL;
    for (size_t i = 0; i < cfg->nrepos; i++) {
        if (strcmp(cfg->repos[i].name, name) == 0)
            return &cfg->repos[i];
    }
    return NULL;
}

static char *url_host(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p)
        return NULL;
    p += 3;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    return gf_strndup(p, len);
}

const gf_repo *gf_config_repo_by_host(const gf_config *cfg, const char *host)
{
    if (!cfg || !host)
        return NULL;
    for (size_t i = 0; i < cfg->nrepos; i++) {
        char *h1 = url_host(cfg->repos[i].api_url);
        char *h2 = url_host(cfg->repos[i].web_url);
        bool match = (h1 && gf_str_ieq(h1, host)) || (h2 && gf_str_ieq(h2, host));
        free(h1);
        free(h2);
        if (match)
            return &cfg->repos[i];
    }
    /* special-case well-known API hosts */
    if (gf_str_ieq(host, "codeload.github.com") ||
        gf_str_ieq(host, "api.github.com"))
        return gf_config_repo(cfg, "github");
    return NULL;
}

const gf_repo *gf_config_repo_enabled(const gf_config *cfg, size_t idx)
{
    size_t seen = 0;
    for (size_t i = 0; i < cfg->nrepos; i++) {
        if (!cfg->repos[i].enabled)
            continue;
        if (seen == idx)
            return &cfg->repos[i];
        seen++;
    }
    return NULL;
}

char *gf_repo_token(const gf_repo *r)
{
    if (!r)
        return NULL;
    if (r->token_file && *r->token_file) {
        size_t len = 0;
        char *tok = gf_fs_read_file_limit(r->token_file, 4096, &len);
        if (tok) {
            gf_str_trim(tok);
            if (*tok)
                return tok;
            free(tok);
        } else {
            gf_log(GF_LOG_WARN, "token_file unreadable: %s", r->token_file);
        }
    }
    const char *envnames[8] = { NULL };
    size_t nenv = 0;
    if (r->token_env && *r->token_env)
        envnames[nenv++] = r->token_env;
    if (gf_str_ieq(r->kind, "github") && nenv < 8) {
        envnames[nenv++] = "GITHUB_TOKEN";
        envnames[nenv++] = "GH_TOKEN";
    }
    if (gf_str_ieq(r->kind, "gitlab") && nenv < 8)
        envnames[nenv++] = "GITLAB_TOKEN";
    if (gf_str_ieq(r->kind, "gitea") && nenv < 8)
        envnames[nenv++] = "GITEA_TOKEN";
    if (nenv < 8)
        envnames[nenv++] = "GITFULL_TOKEN";
    for (size_t i = 0; i < nenv; i++) {
        const char *v = getenv(envnames[i]);
        if (v && *v)
            return gf_strdup(v);
    }
    return NULL;
}

char *gf_repo_web_base(const gf_repo *r)
{
    if (!r)
        return NULL;
    return gf_strdup(r->web_url ? r->web_url : r->api_url);
}

/* ---------------------------------------------------- surgical file edits */

static bool line_starts_repo_section(const char *line, const char *name)
{
    if (*line != '[')
        return false;
    char want[256];
    snprintf(want, sizeof(want), "[repositories.%s]", name);
    if (strncmp(line, want, strlen(want)) == 0)
        return line[strlen(want)] == ']';
    return false;
}

int gf_config_file_repo_remove(const char *path, const char *name)
{
    size_t len = 0;
    char *text = gf_fs_read_file(path, &len);
    if (!text)
        return -1;
    gf_strbuf out;
    gf_strbuf_init(&out);
    bool skipping = false;
    bool removed = false;
    char *copy = gf_strdup(text);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *trimmed = gf_str_trim(line);
        if (*trimmed == '[') {
            skipping = line_starts_repo_section(trimmed, name);
            if (skipping)
                removed = true;
            if (!skipping) {
                gf_strbuf_append(&out, line);
                gf_strbuf_appendc(&out, '\n');
            }
            continue;
        }
        if (skipping)
            continue;
        gf_strbuf_append(&out, line);
        gf_strbuf_appendc(&out, '\n');
    }
    free(copy);
    free(text);
    if (!removed) {
        gf_strbuf_free(&out);
        gf_log(GF_LOG_ERROR, "repository '%s' not found in %s", name, path);
        return -1;
    }
    int rc = gf_fs_write_file_atomic(path, out.s, out.len);
    gf_strbuf_free(&out);
    return rc;
}

int gf_config_file_repo_add(const char *path, const gf_repo *r)
{
    FILE *f = fopen(path, "a");
    if (!f) {
        gf_log_errno(GF_LOG_ERROR, path);
        return -1;
    }
    fprintf(f, "\n[repositories.%s]\n", r->name);
    fprintf(f, "enabled=%s\n", r->enabled ? "true" : "false");
    fprintf(f, "kind=%s\n", r->kind);
    if (r->api_url)
        fprintf(f, "api_url=%s\n", r->api_url);
    if (r->token_file)
        fprintf(f, "token_file=%s\n", r->token_file);
    if (r->token_env)
        fprintf(f, "token_env=%s\n", r->token_env);
    fclose(f);
    gf_fs_fsync_dir(gf_path_dirname(path));
    return 0;
}

int gf_config_validate(const gf_config *cfg)
{
    if (!cfg->state_dir || !*cfg->state_dir)
        return -1;
    if (!cfg->prefix || !gf_path_is_abs(cfg->prefix)) {
        gf_log(GF_LOG_ERROR, "config: prefix must be an absolute path");
        return -1;
    }
    if (cfg->nrepos == 0) {
        gf_log(GF_LOG_ERROR, "config: no repositories configured");
        return -1;
    }
    if (!gf_config_repo(cfg, cfg->default_repo)) {
        gf_log(GF_LOG_ERROR,
               "config: default_repository '%s' is not a configured repository",
               cfg->default_repo);
        return -1;
    }
    for (size_t i = 0; i < cfg->nrepos; i++) {
        const gf_repo *r = &cfg->repos[i];
        if (!gf_str_ieq(r->kind, "github") && !gf_str_ieq(r->kind, "gitlab") &&
            !gf_str_ieq(r->kind, "gitea") && !gf_str_ieq(r->kind, "generic")) {
            gf_log(GF_LOG_ERROR,
                   "config: repository '%s': unknown kind '%s' "
                   "(github|gitlab|gitea|generic)",
                   r->name, r->kind);
            return -1;
        }
        if (!gf_str_starts_with(r->api_url, "https://") && !r->allow_http) {
            gf_log(GF_LOG_ERROR,
                   "config: repository '%s': api_url must be https:// "
                   "(or set allow_http=true for self-hosted)",
                   r->name);
            return -1;
        }
    }
    return 0;
}
