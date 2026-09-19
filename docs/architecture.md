# Architecture

gitfull is a single static CLI binary (~17k lines of C17) with no runtime
daemon. This document maps the modules, the data flow of a package
install, and the reasoning behind the main design decisions.

## Module map

```
src/
  main.c        entry point: global options, config load, dispatch
  cli.c         command table, context, db handle, confirmations
  ops.c         command implementations: install/remove/update/upgrade/
                search/info/list/rollback/hold/history/verify/repo/config
  clean.c       garbage collection (clean, cache)
  doctor.c      diagnostics + interrupted-transaction recovery

  config.c      /etc/gitfull.conf (INI), repository registry, validation
  ini.c         INI parser
  toml.c        TOML-subset parser (recipes)
  json.c        strict JSON parser + canonical serializer
  version.c     semver 2.0.0 + constraint sets + stable-tag selection
  pkgid.c       package reference parsing (owner/repo, forges, URLs, local:)
  recipe.c      gitfull.toml loading + validation
  source.c      resolution policy + source acquisition (tarball/git mirror)
  forge*.c      forge abstraction (vtable) + github/gitlab/gitea/generic
  http.c        curl child process wrapper (argv control, https-only,
                manual redirects, size caps, retries, token scoping)
  gitx.c        git child processes (ls-remote, mirrors, rev-parse,
                verify-tag; ephemeral GIT_ASKPASS credentials)
  detect.c      build-system autodetection + required-tools tables
  toolchain.c   toolchain identity recording (bootstrap/pinned)
  sandbox.c     namespaces, pivot_root, bind mounts, PID-1 reaper
  seccomp.c     syscall filter (allowlist + privilege denylist)
  cgroup.c      cgroup v2 resource limits (when delegated)
  build.c       build orchestration, hermetic environment, build plans
  package.c     .gfpkg create/verify/extract
  tar.c         hardened ustar reader + deterministic writer (gzip via zlib)
  store.c       content-addressed store layout
  db.c          SQLite package database (files, ownership, history,
                rollback slots, holds, transactions)
  xact.c        journaled filesystem transactions + crash recovery
  sha256.c      FIPS 180-4 SHA-256
  util/         mem, str, path, fs, exec, log
```

Dependency policy: link only libc, libsqlite3, libz. `git` and `curl` run as
child processes spawned with fork+execve and explicit argv/envp arrays —
`system()`/`popen()` appear nowhere in the codebase (grep-verifiable).

## Data flow: `gitfull install owner/repo`

```
 1. pkgid.c      parse the reference
 2. source.c     resolve: forge releases/tags -> exact version + commit
                 verification ladder (signature > digest > unsigned)
 3. source.c     acquire: release tarball (checksummed) or git-mirror
                 extraction at the exact commit, into the build tree
 4. recipe.c     read gitfull.toml (if any) -> overrides
 5. depgraph.c   recursive recipe walk -> dependency graph
                 - cycle detection with full chain reporting
                 - version constraint merging + conflict detection
                 - arch/ABI conflict detection
                 - topological install order
 6. build.c      for each package in order:
                 a. toolchain.c  record tool identities
                 b. sandbox.c    userns+mntns+pidns+netns, pivot_root,
                                 read-only system dirs, writable staging
                 c. build plan   autodetected backend or recipe steps
                                 (cmake/meson/make/autotools/cargo/go/
                                  python/node/vala)
                 d. hermetic env SOURCE_DATE_EPOCH, TZ=UTC, LC_ALL=C.UTF-8,
                                 scrubbed PATH, no HOME/token leak
                 e. install into staging/ (DESTDIR semantics)
                 f. tests run inside the same sandbox
 7. package.c    scan staging -> manifest (path/type/mode/size/sha256)
                 pack deterministic .gfpkg (content-addressed build-id)
 8. store.c      place under store/<name>/<version>/<build-id>/
 9. xact.c+db.c  pre-flight file-conflict check
                 journaled filesystem transaction (write-ahead journal +
                 backups + fsync) + SQLite transaction, committed together
10. ops.c        history record; rollback slot for the previous version
```

## Key design decisions

### Isolation without containers

The sandbox (sandbox.c) is built from Linux primitives only:
`clone(CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWIPC|
CLONE_NEWNET)` with parent-written uid/gid maps, a three-level fork
(intermediate status mirror -> PID-1 reaper -> command process) so orphaned
build processes cannot escape the namespace, `pivot_root` with the old root
detached, then the root is **remounted read-only** — only `/staging`, `/tmp`,
`/home/build`, `/var/tmp` are writable. The network namespace is empty by
default (no interfaces), and seccomp denies `mount`, `chroot`, `socket`
family creation for unprivileged build code (verified live by the `evil`
fixture: a program that tries all three must observe all three blocked).

Docker is never required and never invoked.

### Determinism

Every build runs with `SOURCE_DATE_EPOCH` (from the source commit timestamp),
`TZ=UTC`, `LC_ALL=C.UTF-8`, a fixed `PATH`, empty `MAKEFLAGS`, and
`-ffile-prefix-map=/src=.` / `-fdebug-prefix-map=/src=.` so neither absolute
build paths nor timestamps leak into artifacts. The tar writer emits entries
sorted, with uid/gid 0, fixed mtime, sanitized modes. Two builds of the same
tree produce byte-identical archives (unit-tested).

### Content addressing + safe activation

A package's identity is `sha256(pkg.gfpkg)`; the store layout is
`store/<name>/<version>/<build-id>/{pkg.gfpkg,files/}`. Files are activated
from the store with **reflink-or-copy**, never hardlinks: modifying an
installed file in place cannot corrupt the immutable store or the rollback
slots (this is asserted by the integration suite). Disk cost is CoW where the
filesystem supports it.

### Transactions

`xact.c` journals every filesystem mutation *before* performing it
(write-ahead), fsyncs journal and data, and keeps per-step backups. `db.c`
wraps SQLite in matching transactions. Commit order: DB commit, then journal
removal; crash recovery replays or undoes interrupted journals at the next
`gitfull doctor` (unit-tested with a simulated SIGKILL: the original file
must survive).

### Credentials

Tokens live only in environment variables (`token_env` per repository, with
standard fallbacks). They are passed to child processes only when needed:
HTTP via a bearer header emitted for the matching host only, git via an
ephemeral `GIT_ASKPASS` helper file (mode 0600, deleted after use) that reads
the variable — the token never appears in argv, URLs, `.git/config`, config
files or logs. The build sandbox receives a freshly constructed environment
without any credential variables.

### Honest degradation

When the environment cannot provide something, gitfull says so and continues
with the strongest available guarantee: no userns -> env-isolation only
(warned); no /proc mount -> warning per build (hardened kernels); no
delegated cgroups -> resource limits skipped (doctor reports it); pinned
toolchain not populated -> bootstrap fallback (warned, recorded).
