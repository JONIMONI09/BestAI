#!/usr/bin/env bash
# apple_portability_check.sh — feature-test macro hygiene for Apple targets.
#
# Regression for the 2026-10-26 release.yml macOS failure: an
# unconditional in-file `#define _POSIX_C_SOURCE` made the macOS build
# fail (O_NOFOLLOW, MADV_SEQUENTIAL undeclared), because Darwin headers
# treat _POSIX_C_SOURCE as a restriction that hides BSD extensions,
# while glibc treats it as an enablement. The rule this test enforces:
#
#   1. In Apple-configuration preprocessor output the translation unit
#      must NOT define _POSIX_C_SOURCE / _DEFAULT_SOURCE itself.
#   2. In a plain (non-Apple) configuration it MUST (self-containment
#      under strict -std=c99 on glibc is a repo requirement).
#
# The real Apple-syntax proof cannot run on a Linux host: forcing
# -D__APPLE__ on glibc headers is NOT a Darwin simulation (glibc in
# strict -std=c99 mode hides O_CLOEXEC/O_NOFOLLOW/madvise even without
# any feature macro, which is a different restriction than Darwin's).
# That proof is the macos-strict-scan CI job (PR tier) and release.yml's
# build-macos job - both compile the TU with real Darwin headers.
#
# A self-test fixture proves the assertion logic is not vacuous (R25).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

fail() { echo "APPLE-PORABILITY: FAIL - $1"; exit 1; }

# define_count <flags...> -- <file>: how many lines of the preprocessed
# output carry a #define of _POSIX_C_SOURCE or _DEFAULT_SOURCE that the
# source file itself introduced (the headers never define them).
define_count() {
  local file="$1"; shift
  "$@" -E -dD "$file" 2>/dev/null | grep -cE '^#define (_POSIX_C_SOURCE|_DEFAULT_SOURCE)([ (]|$)' || true
}

echo "APPLE-PORABILITY: checking src/hydra_engine.c and src/main.c"

for f in src/hydra_engine.c src/main.c; do
  # (1) Apple configuration: the TU must not self-define the macros.
  n_apple=$(define_count "$f" gcc -std=c99 -Iinclude -D__APPLE__)
  [ "$n_apple" -eq 0 ] || fail "$f defines _POSIX_C_SOURCE/_DEFAULT_SOURCE in Apple configuration ($n_apple defines) - Darwin headers would then hide O_NOFOLLOW/MADV_SEQUENTIAL"

  # (2) Plain configuration: self-containment must hold.
  n_plain=$(define_count "$f" gcc -std=c99 -Iinclude)
  [ "$n_plain" -ge 2 ] || fail "$f is not self-contained under strict C99 (expected both feature-test defines, got $n_plain)"

done

# Self-test: a fixture with the OLD broken pattern (unconditional
# defines) must be flagged by the same assertion logic.
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
cat > "$tmpdir/broken.c" <<'FIXTURE'
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
int main(void) { return 0; }
FIXTURE
n_bad=$(define_count "$tmpdir/broken.c" gcc -std=c99 -D__APPLE__)
[ "$n_bad" -ge 2 ] || fail "self-test vacuous: the assertion did not detect the known-broken fixture ($n_bad defines counted)"

echo "APPLE-PORABILITY: ok (Apple config clean, plain config self-contained, self-test proven; Darwin-header proof = macos-strict-scan CI job + release.yml build-macos)"
