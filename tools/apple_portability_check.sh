#!/usr/bin/env bash
# apple_portability_check.sh — feature-test macro hygiene for Apple targets.
#
# Regression for the 2026-10-26 release.yml macOS failure: an
# unconditional in-file `#define _POSIX_C_SOURCE` made the macOS build
# fail (O_NOFOLLOW, MADV_SEQUENTIAL undeclared), because Darwin headers
# treat _POSIX_C_SOURCE as a restriction that hides BSD extensions,
# while glibc treats it as an enablement.
#
# The assertions are CONTROL-BASED, not absolute: a control translation
# unit with the same includes and no feature defines sets the baseline
# the platform headers themselves produce (glibc's features.h, for
# example, re-emits marker defines of _POSIX_C_SOURCE / _DEFAULT_SOURCE
# once its own feature selection is active). Only the difference between
# the real TU and the control counts.
#
# Rules enforced (host-aware - a plain compile on an Apple host IS the
# Apple configuration):
#   All hosts:
#     1. Apple configuration (forced with -D__APPLE__) adds NO feature
#        define beyond the control baseline.
#     2. A fixture with the old broken pattern (unconditional defines)
#        adds >=2 beyond the baseline - the assertion must detect it
#        (vacuous-parse guard, R25).
#   Non-Apple hosts (Linux CI, this sandbox):
#     3. The plain configuration adds >=2 beyond the baseline
#        (self-containment under strict -std=c99 on glibc).
#   Apple hosts (macOS CI):
#     3'. The plain configuration adds NOTHING beyond the baseline.
#
# The real Darwin-header compile proof cannot run on a Linux host:
# forcing -D__APPLE__ on glibc headers is NOT a Darwin simulation
# (glibc in strict -std=c99 hides O_CLOEXEC/O_NOFOLLOW/madvise even
# without any feature macro - a different restriction than Darwin's).
# That proof is the macos-strict-scan CI job (PR tier) and release.yml's
# build-macos job, both of which compile the TU with real Darwin headers.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

fail() { echo "APPLE-PORABILITY: FAIL - $1"; exit 1; }

# define_count <flags...> -- <file>: how many lines of the preprocessed
# output carry a #define of _POSIX_C_SOURCE / _DEFAULT_SOURCE.
define_count() {
  local file="$1"; shift
  "$@" -E -dD "$file" 2>/dev/null | grep -cE '^#define (_POSIX_C_SOURCE|_DEFAULT_SOURCE)([ (]|$)' || true
}

# Control TU: the same includes the engine files use, no feature defines.
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
cat > "$tmpdir/control.c" <<'CONTROL'
#include "hydra_model.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
CONTROL

# Host class: does this compiler define __APPLE__ out of the box?
APPLE_HOST=0
if printf '' | gcc -dM -E - 2>/dev/null | grep -q 'define __APPLE__'; then
  APPLE_HOST=1
fi
echo "APPLE-PORABILITY: host class: $([ $APPLE_HOST -eq 1 ] && echo Apple || echo non-Apple)"

CTRL_APPLE=$(define_count "$tmpdir/control.c" gcc -std=c99 -Iinclude -D__APPLE__)
CTRL_PLAIN=$(define_count "$tmpdir/control.c" gcc -std=c99 -Iinclude)

for f in src/hydra_engine.c src/main.c; do
  # (1) Apple configuration (forced): adds nothing beyond the control.
  n_apple=$(define_count "$f" gcc -std=c99 -Iinclude -D__APPLE__)
  [ "$n_apple" -eq "$CTRL_APPLE" ] || fail "$f adds feature defines in Apple configuration ($n_apple vs control $CTRL_APPLE) - Darwin headers would then hide O_NOFOLLOW/MADV_SEQUENTIAL"

  # (3)/(3') plain configuration: expectation depends on host class.
  n_plain=$(define_count "$f" gcc -std=c99 -Iinclude)
  if [ "$APPLE_HOST" -eq 1 ]; then
    [ "$n_plain" -eq "$CTRL_PLAIN" ] || fail "$f self-defines feature macros in a plain Apple-host compile ($n_plain vs control $CTRL_PLAIN) - Darwin headers would then hide O_NOFOLLOW/MADV_SEQUENTIAL"
  else
    [ "$n_plain" -ge $((CTRL_PLAIN + 2)) ] || fail "$f is not self-contained under strict C99 on glibc (expected both feature-test defines beyond the control baseline $CTRL_PLAIN, saw $n_plain)"
  fi
done

# (2) Self-test: the OLD broken pattern (unconditional defines) must add
# >=2 beyond the Apple-config control baseline, on every host.
cat > "$tmpdir/broken.c" <<'FIXTURE'
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
int main(void) { return 0; }
FIXTURE
n_bad=$(define_count "$tmpdir/broken.c" gcc -std=c99 -D__APPLE__)
[ "$n_bad" -ge 2 ] || fail "self-test vacuous: the assertion did not detect the known-broken fixture ($n_bad defines counted)"

echo "APPLE-PORABILITY: ok (Apple config adds nothing, plain config host-consistent, self-test proven; Darwin-header proof = macos-strict-scan CI job + release.yml build-macos)"
