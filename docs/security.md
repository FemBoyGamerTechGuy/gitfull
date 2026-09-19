# Security model

This document describes what gitfull defends against, how, and — equally
important — what it does not. The design favors *mechanisms that are actually
enforced by the kernel* over policy documents.

## Threat model

Untrusted inputs gitfull handles:

1. **Forge data** — release listings, tags, repository metadata, tarballs
   from arbitrary hosts.
2. **Source trees** — arbitrary code from any repository.
3. **Package archives** — `.gfpkg` files, potentially corrupted or crafted.
4. **Recipes** — `gitfull.toml` inside untrusted sources.
5. **The local system** — pre-existing files, symlinks, races.

## Defense layers

### No shell, no format strings, no library parser surface

- Zero `system()`/`popen()` (grep-audited). All child processes — git, curl,
  build backends — are spawned via `fork()`+`execve()` with explicit argv
  arrays; output is captured through pipes, never through a shell.
- JSON, TOML-subset, INI, semver and tar are own implementations with strict
  grammar, depth/size limits and no dynamic evaluation — a malformed input
  is a parse error, never a command.
- All printf-style functions carry `__attribute__((format))` and the build
  uses `-Wformat=2 -Werror`.

### Sandboxed builds (the big one)

Every build step executes inside:

```
clone(user | mount | pid | uts | ipc | net namespaces)
  -> parent-written uid_map/gid_map (uid 0 inside maps to the invoking user)
  -> bind mounts:
       /usr /lib /lib64 /bin /sbin /opt ...  read-only (bootstrap toolchain)
       /toolchain                            read-only (recorded tools)
       /src      <- build tree               read-only
       /build    <- build dir                writable
       /staging  <- staging dir              writable
       /deps     <- dependency payload       read-only
       /tmp /home /var/tmp                   tmpfs, writable
       /dev                                 minimal, no device nodes writable
  -> pivot_root(".", "oldroot"), oldroot umounted
  -> / remounted read-only
  -> seccomp filter installed
  -> PID-1 init reaper inside the namespace
```

Enforced (verified live by the `evil` integration fixture, which runs a
program that attempts all of these and must see every one fail):

- `mount()` inside the sandbox: blocked (seccomp).
- Creating inet sockets: blocked (empty network namespace — no interfaces
  exist, `sandbox network=none` by default).
- `chroot()`: blocked (seccomp).
- Writing outside `/build`, `/staging`, tmpfs dirs: impossible (read-only
  root after pivot).
- Host filesystem visibility: only the read-only bind set; no `/proc` when
  the kernel forbids it (warned, never fatal-silently).

The sandbox needs no root and no Docker — it uses unprivileged user
namespaces. If the kernel refuses them, gitfull *degrades loudly*
(`sandbox=none`, big warning, environment still hermetic) rather than
pretending.

### Credential hygiene

- Tokens are read from environment variables (or a 0600 `token_file`); never
  written by gitfull to any file, never placed in URLs or argv.
- HTTP: the bearer header is emitted only for the matching repository host.
- Git: an ephemeral `GIT_ASKPASS` script (created mode 0600, unlinked after
  use) reads the token from the environment; `.git/config` stays clean,
  mirrors use credential-free HTTPS for public repos.
- **The build sandbox env is constructed from scratch**: `PATH`, `HOME`,
  `TMPDIR`, `TZ=UTC`, `LC_ALL=C.UTF-8`, `SOURCE_DATE_EPOCH`,
  `MAKEFLAGS=` — no `GH_TOKEN`, no `HOME`, no host variables leak in.

### Archive hardening

See docs/package-format.md; in short: checksums validated before extraction,
absolute/`..`/escaping-symlink/device-node/external-hardlink/duplicate
entries rejected, O_EXCL creation, setuid+world-write stripped, size and
entry caps.

### Path safety

- `rm -rf` walks with `openat()` + `O_NOFOLLOW` (symlink-substitution
  resistant).
- Store activation uses reflink-or-copy, never hardlinks: the "immutable
  store" cannot be mutated through live files (verified by tamper tests).
- Runtime paths (`root`, `state`, `prefix`) are validated against a
  conservative charset before they are interpolated into build environments;
  where a shell is unavoidable (build-system command strings), values are
  single-quote escaped (`gf_str_shell_quote`, unit-tested against
  `$(...)`, backticks, `;`, pipes).
- TOCTOU resistance: journaling happens *before* mutations; extraction
  refuses non-empty targets.

### Transactions

Write-ahead journal + backups + fsync ordering means a crash at any point
leaves the system in the previous state or mid-state with a complete undo
plan; `gitfull doctor` replays/rolls back interrupted transactions at
startup. DB (SQLite, integrity-checked) and filesystem changes commit
together or not at all. Conflicting files abort installation before any
mutation with exit code 3.

### Process hygiene

- Timeouts on every child (git, curl, build steps — 2h per step by default)
  with process-group kill.
- OOM policy: abort (like dpkg) — allocation helpers are fatal by design,
  keeping every caller auditable.
- Non-root by design: building as root is refused; privileged activation is
  an explicit, separately configured mechanism.

## What is *not* defended (honest list)

- **The build's own output.** A package's build steps run arbitrary code by
  definition; the sandbox limits *where it can write* and *what it can
  touch*, but a hostile package can still waste CPU (cgroup caps only when
  delegated) and produce a hostile binary. Source trust is the release
  channel's job: prefer `verified`/`checksum` sources (see the ladder below)
  and review recipes.
- **Bootstrap toolchain.** In default mode the compilers come from the host
  (bind-mounted read-only, every tool hashed and recorded). A compromised
  host compiler can compromise builds. The fully self-hosted `pinned` mode
  is the escape hatch and is opt-in.
- **Zero-days in the kernel's namespace/seccomp implementation** (same trust
  class as any container runtime).
- **Tag signatures require GPG**: verification runs `gpgv` when present;
  without a web of trust configured, `verified` status is only as strong as
  your keyring.
- **Availability**: forge rate limits can deny service (gitfull retries with
  backoff; a token helps).
- **cgroup resource limits** are only enforced with a delegated cgroup v2
  hierarchy (typical bare-metal root setups); in containers they are
  skipped and reported.

## Source verification ladder

`gitfull info` shows one of:

| status | meaning |
|--------|---------|
| `verified` | tag signature valid (gpgv) |
| `checksum` | release asset digest (sha256) matched |
| `unsigned` | checked; the forge provided nothing to verify |
| `unverified` | verification attempted and inconclusive |
| `edge` | branch head — a moving target by definition |

The level is recorded in package metadata and survives into the database.
