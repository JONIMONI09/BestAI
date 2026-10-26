#!/usr/bin/env bash
# ci_syntax_mirror_test.sh — local/CI syntax parity (regression, 2026-10-26).
#
# Why: PR #55's first CI run failed exactly here — the CI "Syntax check
# (gcc, C99 strict, -Werror)" step compiles FIVE .c files (src + three
# test translation units) with command-line feature-test macros, while
# the local pass had compiled only two files without those flags. The
# in-file defines collided with the command-line ones under -Werror.
#
# This test closes the class of failure: it EXTRACTS the exact command
# from ci.yml at run time (nothing hardcoded) and runs it locally, so
# any future edit of the CI syntax step is automatically mirrored.
# A vacuous parse (no command, or fewer than three .c files referenced)
# is a hard failure, not a pass.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

extract_cmd() { # $1 = step name to locate; prints its `run: |` block
  # mawk-portable awk (no match()/RSTART, no [[:space:]]): counts leading
  # blanks via gsub+length; a block ends at the first non-blank line whose
  # indent drops below the first content line's indent.
  awk -v pat="$1" '
    !in_step && $0 ~ "name: " pat { in_step = 1; next }
    in_step && $1 == "run:" && $2 == "|" { in_run = 1; next }
    in_run && /^[ 	]*$/      { next }
    in_run {
      line = $0
      t = line; gsub(/^[ 	]+/, "", t)
      ws = length(line) - length(t)
      if (indent == 0) {
        indent = ws                       # first content line sets the bar
        print t; next
      }
      if (ws < indent) exit               # back to YAML level: step over
      print t
    }
  ' .github/workflows/ci.yml
}

fail() { echo "CI-SYNTAX-MIRROR: FAIL - $1"; exit 1; }

gcc_cmd=$(extract_cmd "Syntax check \(gcc")
clang_cmd=$(extract_cmd "Syntax check \(clang")
[ -n "$gcc_cmd" ] || fail "could not parse the gcc syntax-check command from ci.yml (step renamed or missing?)"
nfiles=$(printf '%s\n' "$gcc_cmd" | grep -oE '[A-Za-z0-9_./-]+\.c\b' | sort -u | wc -l)
[ "$nfiles" -ge 3 ] || fail "parsed command references only $nfiles .c files - parse is vacuous"

echo "CI-SYNTAX-MIRROR: running the exact ci.yml gcc command on $nfiles .c files"
printf '%s\n' "$gcc_cmd"
if ! eval "$gcc_cmd"; then
  echo "CI-SYNTAX-MIRROR: FAIL - the ci.yml gcc syntax command fails locally"
  exit 1
fi

if command -v clang >/dev/null 2>&1; then
  [ -n "$clang_cmd" ] || fail "could not parse the clang syntax-check command from ci.yml"
  echo "CI-SYNTAX-MIRROR: running the exact ci.yml clang command"
  if ! eval "$clang_cmd"; then
    echo "CI-SYNTAX-MIRROR: FAIL - the ci.yml clang syntax command fails locally"
    exit 1
  fi
else
  echo "CI-SYNTAX-MIRROR: SKIP clang step (no clang in this environment - R23: said out loud, not hidden); gcc mirror proven"
fi

echo "CI-SYNTAX-MIRROR: ok (local run matches the ci.yml commands verbatim)"
