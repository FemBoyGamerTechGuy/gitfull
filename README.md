# gitfull

A source-based package manager for Git forges. `gitfull` installs software the
way modern software is actually distributed: straight from Git repositories and
release tags on GitHub, GitLab, Codeberg and Gitea instances — building each
package in an isolated sandbox, packaging it deterministically, and installing
it transactionally with rollback.

```
$ gitfull install BurntSushi/ripgrep        # newest stable release
$ gitfull install --edge BurntSushi/ripgrep # track the branch head instead
$ gitfull rollback ripgrep                  # back to the previous version
$ gitfull hold ripgrep                      # stop auto-upgrading it
$ gitfull verify --all                      # hash-check every installed file
```

Written in C17. No runtime daemon, no container requirement, no mandatory
root. `git` and `curl` are invoked as controlled child processes (argv arrays,
no shell), SQLite stores the package database, zlib compresses caches.

## Why

- **Forge-native.** A tag on a Git repository is a release. gitfull resolves
  the newest *stable* semver tag by default (`--edge` for branch heads,
  `@v1.2.3` to pin, `--prerelease` to allow rc/beta tags).
- **Sandboxed builds.** Every build runs inside user+mount+PID+network
  namespaces with `pivot_root`, a seccomp syscall allowlist, and read-only
  bind-mounted toolchains — unprivileged, no Docker needed.
- **Reproducible packaging.** Builds run with `SOURCE_DATE_EPOCH`, UTC,
  `C.UTF-8`, deterministic tar writing; every package is content-addressed by
  the SHA-256 of its `.gfpkg` archive.
- **Transactional installs.** Journaled write-ahead filesystem transactions +
  SQLite transactions commit or roll back *together*; a SIGKILL mid-install
  is recovered by `gitfull doctor`.
- **Honest security.** No `system()`/`popen()` anywhere; tokens never touch
  argv, URLs or config files; untrusted tar archives are rejected before
  extraction; store payloads are activated without inode sharing.

## Building

Requirements: a C compiler (GCC 14+ or Clang), GNU make, git, curl, zlib and
SQLite dev headers.

```sh
make            # release build -> ./gitfull
make test       # unit tests + hermetic integration suite
make asan       # ASan + UBSan + LeakSanitizer test build
sudo make install
```

The build is strict: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
-Wformat=2 -Wwrite-strings -Wpointer-arith -Wcast-qual -Wstrict-prototypes
-Wmissing-prototypes -Wswitch-enum -Wundef -Wold-style-definition -Wvla
-Werror` — the code compiles clean or not at all.

## Package references

| Form | Meaning |
|------|---------|
| `owner/repo` | default repository (GitHub unless configured otherwise) |
| `github:owner/repo` `gitlab:...` `codeberg:...` `gitea:...` | specific forge |
| `git+https://host/owner/repo.git` | any HTTPS Git host |
| `https://host/owner/repo` | same |
| `local:/path/to/repo` | local checkout or plain directory |
| *any of the above* `@v1.2.3` | pin an exact tag |

Version resolution policy (identical for remote and local git sources):

1. `@tag` — that exact tag.
2. Default — the newest tag that parses as a **stable** semver release
   (prerelease tags like `v2.0.0-rc1` are skipped).
3. `--prerelease` — prerelease tags allowed.
4. `--edge` — the head of the default branch (recorded as `edge`).

A repository with no usable release tags falls back to `HEAD` with an explicit
notice. Plain directories (no `.git`) are built as-is, version `HEAD`.

Source verification ladder: **verified** (valid tag signature) > **checksum**
(release asset digest matched) > **unsigned** (checked, nothing to verify) >
**unverified** > **edge** (moving target). The level is recorded in the
package metadata and shown by `gitfull info`.

## Commands

```
install    install a package (stable release by default)
remove     remove an installed package
update     refresh metadata and show available upgrades
upgrade    upgrade installed packages (respects holds)
search     search repositories on enabled forges
info       show package information (installed or remote)
list       list installed packages
rollback   restore a previous package version
hold       hold packages (hold/unhold/list subcommands)
history    show install/upgrade/rollback history
verify     verify installed files against the database
doctor     diagnose the gitfull installation
clean      garbage-collect build dirs, caches, old versions
cache      inspect the cache
build      build a package without installing it
repo       manage repositories (list/add/remove/enable/disable)
config     show/validate configuration
logs       show build logs for a package
```

Run `gitfull <command> --help` for flags. Global flags: `--root DIR`
(alternate system root, used by the test suite), `--state DIR`, `--prefix
DIR`, `--yes`, `-v/-q`.

Exit codes are stable and scriptable: 0 ok, 1 general, 2 usage, 3 conflict,
4 network, 5 build failure, 6 test failure, 7 transaction failure, 8 not
found.

## Configuration

`/etc/gitfull.conf` (INI). Repositories, build jobs, sandbox level, rollback
depth, held packages, network timeouts — see **docs/configuration.md**.
Authentication is *never* stored in the config file: set `token_env` per
repository (default `GITFULL_GITHUB_TOKEN`, with `GH_TOKEN`/`GITHUB_TOKEN`/
`GITFULL_TOKEN` fallbacks) and export the variable at runtime.

## Package recipes

Projects may ship a `gitfull.toml` to override autodetection — build steps,
dependencies (runtime/build/optional/test), architecture and ABI constraints,
release channel pins. Simple projects need none: build systems are detected
automatically (CMake, Meson, Make, Autotools, Cargo, Go, Python/wheels, Node,
Vala). See **docs/package-format.md** for the full recipe reference and the
`.gfpkg` archive specification.

## Documentation

- `docs/architecture.md` — module map, data flow, design decisions
- `docs/configuration.md` — every configuration key
- `docs/package-format.md` — `.gfpkg` spec, store layout, `gitfull.toml` recipes
- `docs/security.md` — sandbox layers, threat model, credential handling,
  honest limitations

## Testing

- `tests/unit/` — 507 checks: SHA-256 vectors, semver 2.0.0 ordering and
  constraint sets, INI/TOML/JSON parsers, tar hardening (five classes of
  malicious archives rejected), package create/verify/extract, transaction
  commit/rollback/crash-recovery, seccomp filter, dependency graphs
  (cycles, conflicts, topological order), package-id parsing, shell quoting.
- `tests/integration/run-all.sh` — 72 checks running the real binary: full
  install → verify → upgrade → rollback → remove lifecycle, stable/edge/exact
  version resolution, dependency-driven installs, cycle and architecture
  conflict rejection, live sandbox escape probes (mount/socket/chroot must
  all be blocked), tamper detection and repair, hostile-prefix injection
  rejection, many-file packages, file-conflict transactions.
- `make asan` — the whole unit suite under AddressSanitizer +
  UndefinedBehaviorSanitizer + LeakSanitizer: zero leaks, zero findings.

## Status and limitations

gitfull is young software with an intentionally small honest surface. Things
that work are listed above and verified by tests; the notable boundaries are:

- **Toolchain bootstrap.** gitfull cannot conjure a compiler from nothing. In
  the default `bootstrap` mode host tools are bind-mounted **read-only** into
  the sandbox and every tool's identity (path, version, SHA-256) is recorded
  and verified; the *pinned* fully-self-hosted toolchain mode is an
  implemented extension point but populating it is opt-in and long-running.
- **cgroup limits** (memory/CPU caps) require a delegated cgroup v2
  hierarchy; in containers they are skipped with a warning (recorded by
  `doctor`).
- **Hardened kernels** that forbid unprivileged `/proc` mounts produce a
  degraded-sandbox warning; builds still run namespace-isolated.
- **Offline builds**: `cargo`/`npm`/`pip` dependency fetching needs
  `sandbox network=fetch`; the default is no network in the sandbox.
- Forge API rate limits (especially unauthenticated GitHub secondary limits)
  can slow `search`/`info`; exporting a token fixes it.

License: MIT.
