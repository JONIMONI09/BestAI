#!/usr/bin/env bash
# Test-count drift gate.
#
# WHY THIS EXISTS
# ---------------
# rules.md R21 used to hard-code "49 tests". It was wrong for years: the suite
# grew, the ARM build legitimately runs MORE assertions than the x86 build
# (the NEON-vs-scalar comparison tests are compiled out on x86), and
# `-DHYDRA_TOKENV_MASK` builds report yet another count. A stale literal in a
# doc is harmless; a stale literal in a CI gate means the gate is either
# permanently red or permanently green and catches nothing.
#
# So no number is hard-coded HERE. The counts live in one reviewed file,
# tools/test_count_baseline.txt, keyed by architecture. The gate:
#   1. runs the binary and parses "<N> Tests, <F> failures" from its output;
#   2. fails if F != 0;
#   3. fails if N differs from the baseline for that architecture.
#
# Changing the count is therefore a deliberate, reviewable edit to the baseline
# file, not a silent change that ships.
#
# Usage: test_count_check.sh <binary> <arch-label> [runner ...]
#   bash tools/test_count_check.sh ./hydra-test x86
#   bash tools/test_count_check.sh ./hydra-test-arm arm qemu-aarch64-static
#
# The optional trailing words are the COMMAND PREFIX used to execute the
# binary. They are not optional in CI: the ARM job builds an AArch64 binary
# and the runner is x86-64, so executing the binary directly depends on
# binfmt_misc being registered, which a GitHub runner does not guarantee.
# Passing `qemu-aarch64-static` makes the job depend on the emulator instead
# of on the host's binfmt registration.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="$ROOT/tools/test_count_baseline.txt"
BIN="${1:-}"
ARCH="${2:-}"
shift 2 2>/dev/null || true
RUNNER=("$@")

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -n "$BIN" ] || fail "usage: test_count_check.sh <binary> <arch-label> [runner ...]"
[ -x "$BIN" ] || fail "not an executable test binary: $BIN"
[ -n "$ARCH" ] || fail "no architecture label given"
[ -f "$BASELINE" ] || fail "missing baseline file: $BASELINE"

# Run it and capture stdout+stderr: the summary line goes to stdout, but a
# crash could put it anywhere.
OUT="$("${RUNNER[@]}" "$BIN" 2>&1)"
STATUS=$?

# Parse the last "<N> Tests, <F> failures" line.
LINE="$(printf '%s\n' "$OUT" | grep -E '[0-9]+ Tests?, [0-9]+ failures?' | tail -1)"
[ -n "$LINE" ] || fail "no '<N> Tests, <F> failures' line in the output of $BIN"

COUNT="$(printf '%s' "$LINE" | sed -E 's/.*[^0-9]([0-9]+) Tests?,.*/\1/')"
FAILS="$(printf '%s' "$LINE" | sed -E 's/.*, *([0-9]+) failures?.*/\1/')"

if [ "$STATUS" -ne 0 ]; then
  printf '%s\n' "$OUT" | tail -20 >&2
  fail "$BIN exited $STATUS"
fi
[ "$FAILS" -eq 0 ] || fail "$BIN reported $FAILS failing tests: $LINE"

# Read the baseline for this architecture.
EXPECTED="$(awk -F: -v a="$ARCH" '$1 ~ "^"a"$" { gsub(/[[:space:]]/,"",$2); print $2 }' "$BASELINE")"
[ -n "$EXPECTED" ] || fail "no baseline entry for architecture '$ARCH' in $BASELINE"

if [ "$COUNT" != "$EXPECTED" ]; then
  fail "assertion-count drift on '$ARCH': parsed $COUNT, baseline $EXPECTED.
       If tests were added or removed deliberately, update
       $BASELINE in the same commit (one line per architecture)."
fi

echo "ok   test count $COUNT (arch '$ARCH'), 0 failures, matches baseline"
