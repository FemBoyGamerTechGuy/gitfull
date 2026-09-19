# Configuration

gitfull reads `/etc/gitfull.conf` (INI syntax: `[section]`, `key = value`,
comments with `#` or `;`). With `--root DIR` the path becomes
`DIR/etc/gitfull.conf`; `--config PATH` overrides it entirely. When no file
exists, built-in defaults are used and reported as such by
`gitfull config show`.

`gitfull config show` prints the effective configuration,
`gitfull config validate` checks a file, `gitfull config path` prints the
path in use.

## Sections and keys

### [general]

| key | default | meaning |
|-----|---------|---------|
| `default_repository` | `github` | repository used for bare `owner/repo` references |
| `architecture` | detected (`x86_64`) | host architecture; recipes with a different `[package] arch` are rejected |

### [build]

| key | default | meaning |
|-----|---------|---------|
| `prefix` | `/usr/local` | installation prefix. With `--root`, the default becomes `<root>/usr/local`. Paths must be absolute and use a conservative charset (letters, digits, `.`, `_`, `-`, `+`, `/`) — anything else is rejected because these values are interpolated into build environments |
| `jobs` | `0` | parallel build jobs (`0` = auto) |
| `run_tests` | `true` | run package test suites during builds (per-recipe override: `[build] run_tests` in gitfull.toml) |
| `toolchain_mode` | `bootstrap` | `bootstrap` (host tools bind-mounted read-only, identity recorded) or `pinned` (self-hosted toolchain tree; falls back to bootstrap with a warning when not populated) |
| `memory_limit_mb` | `0` | cgroup v2 memory cap for builds (`0` = off; needs a delegated hierarchy) |
| `cpu_quota_percent` | `0` | cgroup v2 CPU quota (`0` = off) |

### [sandbox]

| key | default | meaning |
|-----|---------|---------|
| `level` | `userns` | `userns` (full namespaces), `chroot` (pivot_root only), `none` (environment isolation only; warned loudly) |
| `network` | `none` | `none` (empty network namespace) or `fetch` (network reachable during builds — needed by cargo/npm/pip dependency fetching) |
| `seccomp` | `true` | syscall filter (allowlist + privilege/escape denylist) |

### [rollback]

| key | default | meaning |
|-----|---------|---------|
| `hold` | `3` | previous package *versions* kept in the store per package for `gitfull rollback` (garbage-collected beyond this by `gitfull clean`) |

### [hold]

| key | default | meaning |
|-----|---------|---------|
| `packages` | empty | comma-separated package names held back from automatic upgrades (managed at runtime by `gitfull hold/unhold`) |

### [network]

| key | default | meaning |
|-----|---------|---------|
| `timeout_secs` | `120` | HTTP timeout per request |
| `max_download_mb` | `2048` | hard size cap for downloads |
| `retries` | `2` | retry attempts with backoff (429/5xx aware) |

### [privilege]

| key | default | meaning |
|-----|---------|---------|
| `escalation` | none | reserved: how activation into a root requiring privileges is obtained (`sudo`/`doas` when explicitly configured). Default: gitfull refuses to write where it has no permission |

### [repositories.NAME]

One section per forge. Built-ins: `github` (enabled), `codeberg` (disabled),
`gitlab` (disabled). `gitfull repo add/remove/enable/disable` edits these.

| key | meaning |
|-----|---------|
| `kind` | `github` \| `gitlab` \| `gitea` \| `generic` |
| `enabled` | `true`/`false` |
| `api_url` | API base, e.g. `https://api.github.com`, `https://codeberg.org/api/v1`, `https://gitlab.com/api/v4`, or your self-hosted instance |
| `token_env` | environment variable holding the access token |
| `token_file` | optional file to read the token from (must be mode 0600) |

## Authentication

Tokens are never written by gitfull. Per repository, the lookup order is:

1. `token_file` (if configured and readable)
2. `token_env` variable
3. kind-specific fallbacks: `GITHUB_TOKEN`/`GH_TOKEN` (github),
   `GITLAB_TOKEN` (gitlab), `GITEA_TOKEN` (gitea)
4. `GITFULL_TOKEN` (any kind)

The token is attached only to requests against that repository's host. For
git operations an ephemeral `GIT_ASKPASS` helper (0600, unlinked after use)
reads it from the environment. See `docs/security.md`.

## Environment overrides

| variable | effect |
|----------|--------|
| `GITFULL_ROOT` | default `--root` |
| `GITFULL_STATE_DIR` | default `--state` |
| `GITFULL_CONFIG` | default `--config` |
| `GITFULL_ONLINE` | non-empty: `gitfull doctor` probes forge connectivity |

## Command-line overrides

`--root`, `--config`, `--state`, `--prefix` override the configuration;
`--prefix` values (like config `prefix`) are validated against the
conservative path charset. Command options (e.g. `install --edge`) come
*after* the command; global options come *before* it.
