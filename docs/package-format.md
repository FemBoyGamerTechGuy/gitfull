# Package format, store and recipes

## The `.gfpkg` archive

A `.gfpkg` file is a deterministic, uncompressed POSIX ustar archive
(optionally gzip-compressed on read; payloads are written uncompressed for
determinism) with a fixed member layout:

```
METADATA     canonical JSON (sorted keys): name, version, arch, forge,
             repo_url, commit, source_sha, source_status (verified |
             checksum | unsigned | unverified | edge), description, license,
             build_id, pkg_sha, dependency closure, toolchain identity
MANIFEST     canonical JSON array, one entry per payload path:
             {path, type, mode, size, sha256, target}
CHECKSUMS    "sha256  <path>" lines (human-readable mirror of MANIFEST)
payload/...  the staged file tree
```

Properties:

- **Content-addressed identity.** `build_id == sha256(the whole .gfpkg)`.
  Two builds of the same source under the same toolchain identity produce
  the same bytes and therefore the same identity.
- **Deterministic writer.** Entries are emitted in sorted order with
  uid/gid 0, uname/gname empty, mtime = `SOURCE_DATE_EPOCH` (commit
  timestamp) or 0, and sanitized modes: setuid/setgid and world-write bits
  are stripped. Byte-identity of two builds is unit-tested.
- **Hardened reader.** Archives are validated *before* extraction: header
  checksums, member sizes, entry-count/total-size caps. Rejected during
  extraction: absolute paths, `..` components, symlinks whose target escapes
  the extraction root, device nodes, hardlinks to paths outside the archive,
  duplicate paths (O_EXCL create — TOCTOU-resistant). All five malicious
  classes are covered by unit tests built with Python's `tarfile`.
- **Post-extraction verification.** `gitfull verify` re-hashes every
  installed file against the MANIFEST.

## Store layout

```
<state>/store/<name>/<version>/<build-id>/pkg.gfpkg
<state>/store/<name>/<version>/<build-id>/files/        # extracted payload
```

A version directory can hold several build-ids (e.g. a rebuilt package whose
inputs changed). Activation installs files from `files/` with
**reflink-or-copy** — never hardlinks — so in-place modification of a live
file can never write back into the immutable store (integration-tested).
Previous versions (up to `[rollback] hold`, default 3) remain in the store
as rollback slots; `gitfull clean` garbage-collects beyond that.

## `gitfull.toml` recipes

A recipe is an *override* mechanism; autodetection remains the default and
simple projects need none. Place `gitfull.toml` in the repository root (or
in the `[source] subdir`).

```toml
[package]
name = "hello"                  # override the derived package name
description = "greeting program"
license = "MIT"
homepage = "https://example.com"
arch = "x86_64"                 # optional: must match the host
abi = "c11-v1"                  # optional: pairwise conflict detection

[source]
subdir = "src/hello"            # when the repo root != source root

[build]
system = "make"                 # cmake|meson|ninja|make|autotools|cargo|
                                # go|python|node|vala|custom (override detect)
jobs = 2                        # override parallel jobs
run_tests = false               # override global run_tests

[build.env]
CC = "cc"                       # extra environment (applied last)
CFLAGS = "-O2 -g"

[[build.steps]]                 # custom build plan (system = "custom" or
argv = ["make", "-j2"]          # mixed with detected backends)
cwd = "src"                     # build (default) | src | relative path
desc = "compile"

[[build.install_steps]]
argv = ["make", "DESTDIR=/staging", "install"]

[[build.test_steps]]
argv = ["./bin/hello"]

[dependencies]                  # runtime
libgreeting = "local:../libgreeting"
openssl = "github:openssl/openssl@3.3.1"
tool = "^1.2"                   # constraint against default forge

[build-dependencies]            # present at build time, not installed
[optional-dependencies]         # skipped when unresolvable
[test-dependencies]             # only when tests run

[release]
pin_tag = "v1.2.3"              # exact tag hint (recipe-level pin)
channel = "stable"              # stable | prerelease | edge
```

Dependency spec grammar (version.c):

- A **package reference** (contains `/`, `://`, or `local:`): the source of
  the dependency, optionally with `@version`.
- Otherwise a **constraint set** against the default repository:
  `=, ==, >=, <=, >, <, !=, ^, ~` operators, wildcards (`1.2.x`, `1.x`, `*`),
  comma-separated conjunctions (`>=1.2,<2`).

Resolution combines all requesters' constraints for a package; unsatisfiable
sets are reported as version conflicts with both sides. Cycles are reported
with the full chain (`a -> b -> c -> a`). Architecture mismatches and
incompatible ABI tags are hard errors. Install order is topological
(dependencies first, diamond dependencies deduplicated).

## Build-system backends

Detection order and required tools (detect.c):

| detected via | backend | steps (inside sandbox) |
|--------------|---------|------------------------|
| `CMakeLists.txt` | cmake | `cmake -S /src -B /build -G Ninja`, `ninja`, `ninja install`, `ctest` |
| `meson.build` | meson | `meson setup`, `ninja`, `DESTDIR=/staging ninja install`, `meson test` |
| `configure.ac`/`configure` | autotools | `sh /src/configure --prefix=...`, `make`, `DESTDIR=/staging make install`, `make check` |
| `Makefile` | make | `make -j`, `make DESTDIR=/staging PREFIX=... install`, `make check` |
| `Cargo.toml` | cargo | `cargo build --release --offline`, `cargo install --root /staging/...`, `cargo test` |
| `go.mod` | go | `go build -trimpath`, staged binary, `go test` |
| `setup.py`/`pyproject.toml` | python | `pip wheel`, `pip install --prefix /staging`, `pytest` |
| `package.json` | node | `npm ci --ignore-scripts`, `npm pack`, tar staging, `npm test` |
| Vala project files | vala (meson) | as meson |

Notes: `PREFIX` is passed in both spellings (`PREFIX=` and `prefix=`) so
either Makefile convention is honored. Prefix values are single-quoted for
the shells that build systems spawn, and additionally validated at config
load against a conservative charset (see docs/configuration.md).
