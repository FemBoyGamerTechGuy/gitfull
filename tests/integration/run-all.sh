#!/usr/bin/env bash
# gitfull integration test suite — hermetic (no network required).
#
# Runs the real gitfull binary against local fixtures under a throwaway
# --root, exercising the full lifecycle: resolve -> sandboxed build ->
# package -> transactional install -> verify -> upgrade -> rollback ->
# hold -> remove, plus dependency graphs, cycle/arch conflicts, the
# sandbox escape probe, prefix handling, and shell-injection hardening.
#
# Usage: bash tests/integration/run-all.sh [binary]
#   binary defaults to ./gitfull (built by `make`).

set -u

BIN="${1:-./gitfull}"
if [ ! -x "$BIN" ]; then
    echo "gitfull binary not found at $BIN (run: make)" >&2
    exit 1
fi
case "$BIN" in
    /*) ;;
    *) BIN="$PWD/$BIN" ;;
esac

FIX="tests/fixtures"
TMPROOT="$(mktemp -d /tmp/gitfull-int-XXXXXX)"
trap 'rm -rf "$TMPROOT" /tmp/gitfull-int-gitfix-XXXXXX 2>/dev/null' EXIT

PASS=0
FAIL=0
CURRENT=""

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$CURRENT"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s: %s\n' "$CURRENT" "${1:-}"; }
note() { printf '       %s\n' "$*"; }

# check <desc> <cmd...>: command must exit 0
check() {
    CURRENT="$1"; shift
    if "$@" >/dev/null 2>&1; then ok; else bad "exit $?"; fi
}
# expect_fail <desc> <expected-exit> <cmd...>
expect_fail() {
    CURRENT="$1"; local want="$2"; shift 2
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then ok; else bad "exit $got, want $want"; fi
}
# expect_out <desc> <pattern> <cmd...>: exit 0 AND output matches pattern
expect_out() {
    CURRENT="$1"; local pat="$2"; shift 2
    local out
    out="$("$@" 2>&1)"
    if [ $? -eq 0 ] && printf '%s' "$out" | grep -qE "$pat"; then
        ok
    else
        bad "output/pattern mismatch: $pat"
        printf '%s\n' "$out" | sed 's/^/         | /' | head -5
    fi
}

root() { mktemp -d "$TMPROOT/root-XXXXXX"; }

echo "== gitfull integration suite ($BIN) =="

# ---------------------------------------------------------------- smoke ----
echo "-- smoke"
check "--help exits 0"              "$BIN" --help
check "--version exits 0"           "$BIN" --version
expect_fail "bad command exits 2"   2 "$BIN" frobnicate
R=$(root)
expect_out "install --help"         "Usage|install" "$BIN" install --help
expect_out "list on empty root"     "no packages|PACKAGE" "$BIN" --root "$R" list
expect_out "doctor runs"            "doctor|check" "$BIN" --root "$R" doctor
expect_out "config show"            "architecture|prefix" "$BIN" --root "$R" config show
expect_fail "bare config is usage"  2 "$BIN" --root "$R" config
expect_out "repo list"              "github" "$BIN" --root "$R" repo list

# ------------------------------------------------------------- lifecycle ----
echo "-- lifecycle (local dir fixture)"
R=$(root)
check "install hello"               "$BIN" --root "$R" --yes install "local:$FIX/hello"
check "installed binary runs"       "$R/usr/local/bin/hello"
expect_out "list shows hello"       "hello" "$BIN" --root "$R" list
expect_out "history records install" "installed" "$BIN" --root "$R" history
check "verify --all passes"         "$BIN" --root "$R" verify --all
check "verify <pkg> passes"         "$BIN" --root "$R" verify hello

# tampering must be detected
echo "tampered" > "$R/usr/local/bin/hello"
CURRENT="verify detects tampering"
if "$BIN" --root "$R" verify --all >/dev/null 2>&1; then
    bad "verify passed on tampered file"
else ok; fi
STOREBIN=$(find "$R/var/lib/gitfull/store/hello" -path "*/files/usr/local/bin/hello" | head -1)
CURRENT="store payload pristine after live tampering"
if [ -n "$STOREBIN" ] && ! grep -q "^tampered$" "$STOREBIN"; then ok
else bad "tampering propagated into the store"; fi
# --force re-activates from the (pristine) store: the repair path
check "install --force repairs tampering" \
    "$BIN" --root "$R" --yes install --force "local:$FIX/hello"
check "verify passes after repair"      "$BIN" --root "$R" verify --all

# rollback: install two versions of the same package name (hello2 replaces)
R=$(root)
check "install hello (v1)"          "$BIN" --root "$R" --yes install "local:$FIX/hello"
check "install hello2 (v2 payload)" "$BIN" --root "$R" --yes install "local:$FIX/hello2"
CURRENT="hello2 payload active"
if "$R/usr/local/bin/hello" | grep -q "v2"; then ok; else bad "payload mismatch"; fi
check "rollback to previous"        "$BIN" --root "$R" --yes rollback hello
CURRENT="rollback restored v1 payload"
if "$R/usr/local/bin/hello" | grep -qv "v2"; then ok; else bad "payload not restored"; fi
check "remove"                      "$BIN" --root "$R" --yes remove hello
CURRENT="binary removed"
if [ ! -e "$R/usr/local/bin/hello" ]; then ok; else bad "file still present"; fi
expect_out "history shows remove"   "removed" "$BIN" --root "$R" history

# hold/unhold
R=$(root)
check "install hello"               "$BIN" --root "$R" --yes install "local:$FIX/hello"
check "hold hello"                  "$BIN" --root "$R" hold hello
expect_out "hold list"              "hello" "$BIN" --root "$R" hold list
expect_out "update marks HOLD"      "HOLD" "$BIN" --root "$R" update
check "unhold hello"                "$BIN" --root "$R" unhold hello
expect_out "update no longer HOLD"  "hello .*up to date" "$BIN" --root "$R" update

# ------------------------------------------------------- versioned source ----
echo "-- versioned git source"
GITFIX="$(mktemp -d /tmp/gitfull-int-gitfix-XXXXXX)"
(
    cd "$GITFIX"
    cp -r "$OLDPWD/$FIX/hello/"* .
    git init -q .
    git config user.email test@gitfull.invalid
    git config user.name "gitfull test"
    printf '#include <stdio.h>\nint main(void){puts("one");return 0;}\n' > src/main.c
    git add -A && git commit -qm v1 && git tag v1.0.0
    printf '#include <stdio.h>\nint main(void){puts("two");return 0;}\n' > src/main.c
    git add -A && git commit -qm v2 && git tag v1.1.0
    printf '#include <stdio.h>\nint main(void){puts("rc");return 0;}\n' > src/main.c
    git add -A && git commit -qm rc && git tag v2.0.0-rc1
) >/dev/null 2>&1

R=$(root)
expect_out "stable default picks v1.1.0 (not rc)" "resolving.*v1\.1\.0" \
    "$BIN" --root "$R" --yes install "local:$GITFIX"
CURRENT="stable binary is v1.1.0"
if [ "$("$R/usr/local/bin/hello")" = "two" ]; then ok; else bad "wrong version active"; fi
R=$(root)
expect_out "--edge uses branch head" "resolving.*edge" \
    "$BIN" --root "$R" --yes install --edge "local:$GITFIX"
R=$(root)
expect_out "@exact pins v1.0.0" "resolving.*v1\.0\.0" \
    "$BIN" --root "$R" --yes install "local:$GITFIX@v1.0.0"
CURRENT="exact binary is v1.0.0"
if [ "$("$R/usr/local/bin/hello")" = "one" ]; then ok; else bad "wrong version active"; fi
R=$(root)
expect_fail "unknown tag fails (network-ish exit)" 8 \
    "$BIN" --root "$R" --yes install "local:$GITFIX@v9.9.9"

# upgrade + rollback across tags on one root
R=$(root)
check "install pinned v1.0.0"       "$BIN" --root "$R" --yes install "local:$GITFIX@v1.0.0"
# newer stable tag exists (v1.1.0)
expect_out "update finds v1.1.0"    "update available \(v1\.1\.0\)" \
    "$BIN" --root "$R" update
check "upgrade to v1.1.0"           "$BIN" --root "$R" --yes upgrade
CURRENT="upgraded binary is v1.1.0"
if [ "$("$R/usr/local/bin/hello")" = "two" ]; then ok; else bad "upgrade did not activate"; fi
check "rollback --to v1.0.0"        "$BIN" --root "$R" --yes rollback hello --to v1.0.0
CURRENT="rollback binary is v1.0.0"
if [ "$("$R/usr/local/bin/hello")" = "one" ]; then ok; else bad "rollback did not activate"; fi
expect_out "update stays quiet at pinned old version + newer" \
    "update available" "$BIN" --root "$R" update

# --------------------------------------------------------- dependencies ----
echo "-- dependency graph"
R=$(root)
check "install app with dependency" "$BIN" --root "$R" --yes install "local:$FIX/app"
CURRENT="dependency installed first (libgreeting)"
if [ -e "$R/usr/local/lib/libgreeting.a" ]; then ok; else bad "libgreeting missing"; fi
CURRENT="app runs against dependency"
if "$R/usr/local/bin/app" | grep -q greeting; then ok; else bad "app failed"; fi
expect_out "app info lists dependency" "libgreeting" "$BIN" --root "$R" info app

echo "-- conflict detection"
expect_fail "cycle detected (conflict exit)" 3 \
    "$BIN" --root "$(root)" --yes install "local:$FIX/cyc-a"
CURRENT="cycle message names the chain"
if "$BIN" --root "$(mktemp -d "$TMPROOT/c-XXXXXX")" --yes install "local:$FIX/cyc-a" 2>&1 \
    | grep -q "cyc-a -> cyc-b -> cyc-a"; then ok; else bad "chain not shown"; fi
expect_fail "arch mismatch rejected" 3 \
    "$BIN" --root "$(root)" --yes install "local:$FIX/wrongarch"

# ------------------------------------------------------------- sandbox -----
echo "-- sandbox escape probe"
R=$(root)
check "evil probe package builds"   "$BIN" --root "$R" --yes install "local:$FIX/evil"
STEPS=$(find "$R/var/lib/gitfull/builds/evil" -name steps.jsonl | head -1)
CURRENT="seccomp blocks mount/socket/chroot"
if [ -n "$STEPS" ] && grep -q "mount blocked" "$STEPS" \
   && grep -q "socket blocked" "$STEPS" && grep -q "chroot blocked" "$STEPS"; then
    ok
else
    bad "probe results not found in $STEPS"
fi
CURRENT="no UNFILTERED escape in probe output"
if [ -n "$STEPS" ] && ! grep -q "UNFILTERED" "$STEPS"; then ok; else bad "escape succeeded!"; fi
CURRENT="no writes outside root"
if [ ! -e /pwned ] && [ ! -e /staging ]; then ok; else bad "host filesystem modified"; fi

# ------------------------------------------------------------- prefix ------
echo "-- prefix handling"
R=$(root)
check "custom --prefix installs there" \
    env -u GITFULL_TOKEN "$BIN" --root "$R" --yes --prefix /opt/gitfull-it install "local:$FIX/hello"
CURRENT="binary under custom prefix"
if [ -x "$R/opt/gitfull-it/bin/hello" ]; then ok; else bad "missing under prefix"; fi

echo "-- shell injection hardening (prefix charset validation)"
R=$(root)
CURRENT="hostile prefix rejected at config validation"
if env -u GITFULL_TOKEN "$BIN" --root "$R" --yes --prefix "/usr/x; touch /gitfull-pwned" \
    install "local:$FIX/hello" >/dev/null 2>&1; then
    bad "hostile prefix was accepted"
else ok; fi
CURRENT="no injection artifact on host"
if [ ! -e /gitfull-pwned ]; then ok; else bad "injection executed on host!"; fi
rm -f /gitfull-pwned

# ------------------------------------------------------------ big tree -----
echo "-- many-file package"
R=$(root)
check "bigdata builds+installs"     "$BIN" --root "$R" --yes install "local:$FIX/bigdata"
check "bigdata verifies"            "$BIN" --root "$R" verify bigdata

# --------------------------------------------------------- transactions ----
echo "-- file conflicts"
R=$(root)
mkdir -p "$R/usr/local/bin"
echo "foreign" > "$R/usr/local/bin/hello"
expect_fail "unowned file conflicts (exit 3)" 3 \
    "$BIN" --root "$R" --yes install "local:$FIX/hello"
CURRENT="foreign file untouched after failed install"
if [ "$(cat "$R/usr/local/bin/hello")" = "foreign" ]; then ok; else bad "file was modified"; fi

echo "-- build without install"
R=$(root)
expect_out "build only (no activation)" "built without installing" \
    "$BIN" --root "$R" --yes build "local:$FIX/hello2"
CURRENT="nothing activated by build"
if [ ! -e "$R/usr/local/bin/hello" ]; then ok; else bad "build activated files"; fi

# --------------------------------------------------------------- cache -----
echo "-- cache/clean/logs"
R=$(root)
check "install hello"               "$BIN" --root "$R" --yes install "local:$FIX/hello"
expect_out "cache reports store"    "store:" "$BIN" --root "$R" cache
expect_out "logs show build steps"  "built hello" "$BIN" --root "$R" logs hello
check "clean runs"                  "$BIN" --root "$R" clean --builds
check "verify after clean"          "$BIN" --root "$R" verify --all

# ---------------------------------------------------------------- done -----
echo
echo "----"
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
