/* config.h — gitfull configuration (/etc/gitfull.conf) and path resolution. */
#ifndef GF_CONFIG_H
#define GF_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct gf_repo {
    char *name;        /* config name, e.g. "github", "codeberg", "example" */
    char *kind;        /* github | gitlab | gitea | generic */
    char *api_url;     /* API base URL (generic: same as web url) */
    char *web_url;     /* web base, e.g. https://github.com */
    bool enabled;
    bool allow_http;   /* only meaningful for self-hosted */
    char *token_file;  /* optional: file containing a bearer token */
    char *token_env;   /* optional: env var holding a bearer token */
    char *tls_cafile;  /* optional: CA bundle for private-PKI forges */
} gf_repo;

typedef struct gf_config {
    char *path;            /* config file actually used (may be NULL) */
    char *root;            /* alternative system root ("" = real /) */
    char *arch;            /* e.g. x86_64 */
    char *state_dir;       /* <root>/var/lib/gitfull */
    char *prefix;          /* <root>/usr/local */
    char *etc_conf;        /* <root>/etc/gitfull.conf */
    char *default_repo;    /* name of default repository */
    char *toolchain_mode;  /* bootstrap | pinned */
    char *sandbox_level;   /* userns | chroot | none */
    char *sandbox_network; /* none | fetch (network only during dep fetch) */
    bool sandbox_seccomp;  /* install the syscall filter in builds */
    int memory_limit_mb;   /* cgroup v2 memory limit for builds (0 = off) */
    int cpu_quota_percent; /* cgroup v2 cpu quota for builds (0 = off) */
    char *log_level;       /* debug | info | warn | error */
    char *escalation;      /* auto | sudo | doas | none */
    int rollback_hold;     /* retained rollback versions (default 3) */
    int jobs;              /* parallel build jobs */
    int timeout_secs;      /* network timeout */
    int max_download_mb;   /* download size cap */
    int retries;           /* network retries */
    bool run_tests;        /* run package tests during build */
    bool interactive;      /* ask before applying transactions */
    char **holds;          /* config-seeded holds */
    size_t nholds;
    gf_repo *repos;
    size_t nrepos;
    size_t nrepos_cap;    /* allocated slots */
} gf_config;

/* Load configuration. root: alternative system root or NULL. config_path:
 * explicit --config path or NULL (then <root>/etc/gitfull.conf, then
 * GITFULL_CONFIG). When no file exists, built-in defaults are used and
 * path stays NULL. Returns NULL on fatal errors (bad INI, bad values). */
gf_config *gf_config_load(const char *root, const char *config_path);
void gf_config_free(gf_config *cfg);
/* Validate runtime paths (root/state/prefix) against a shell-safe,
 * path-safe charset. These values are interpolated into build environments
 * (CFLAGS/LDFLAGS, make command lines); unsafe characters are rejected
 * loudly rather than half-quoted through a dozen build systems.
 * Call AFTER all CLI overrides. 0/-1. */
int gf_config_validate_runtime(const gf_config *cfg);

/* Find a repository config by name. */
const gf_repo *gf_config_repo(const gf_config *cfg, const char *name);
/* Find a repository config matching a host (api host or web host). */
const gf_repo *gf_config_repo_by_host(const gf_config *cfg, const char *host);
/* Enabled repositories, in config order. */
const gf_repo *gf_config_repo_enabled(const gf_config *cfg, size_t idx);

/* Resolve the effective token for a repo (token_file > token_env > kind
 * default env > GITFULL_TOKEN). Returns malloc'd token or NULL. */
char *gf_repo_token(const gf_repo *r);

/* Web base URL for a repo (malloc'd). */
char *gf_repo_web_base(const gf_repo *r);

/* Default /etc/gitfull.conf contents (as used when none exists). */
const char *gf_config_default_text(void);

/* Surgical file edits for repo add/remove: preserve unrelated lines. */
int gf_config_file_repo_remove(const char *path, const char *name);
int gf_config_file_repo_add(const char *path, const gf_repo *r);

/* Validate: returns 0 when all values are sane; -1 (message logged) if not. */
int gf_config_validate(const gf_config *cfg);

/* Machine string (uname -m). */
const char *gf_config_default_arch(void);

#endif /* GF_CONFIG_H */
