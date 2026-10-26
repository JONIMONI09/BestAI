#!/usr/bin/env bash
# Gate 1j: the R33 session-start marker in .agents/session.md.
#
# R33 (rules.md): before the first code edit of a session the agent
# reads AGENTS.md + the .agents/ files and writes a session-start
# marker into the CURRENT round section of session.md:
#
#   **Session start (R33):** <date> (branch ...). Read: <file list>.
#
# Why a gate: 2026 context research (see the R33 entry in rules.md and
# the 2026-10-26 errors.md entry) shows prose-only rules are followed
# probabilistically and more often skipped as the session/work grows
# (context dilution). "Instructions raise the probability of the
# behaviour you want; only enforcement guarantees it." This gate makes
# a skipped read a RED check, not a habit.
#
# R25 negative control runs in-gate: a round section WITHOUT the
# marker must be rejected, one WITH it must be accepted — a detector
# that cannot tell the two apart fails the gate.
set -euo pipefail
cd "$(dirname "$0")/.."

SESSION=.agents/session.md
if [ ! -f "$SESSION" ]; then
  echo "SESSION-START-CHECK: FAIL - $SESSION missing" >&2
  exit 1
fi

# has_marker <file> -> 0 if the LAST '## Round' section carries the R33 marker.
has_marker() {
  local f="$1" line
  line=$(grep -n '^## Round' "$f" | tail -1 | cut -d: -f1) || return 1
  sed -n "${line},\$p" "$f" \
    | awk 'NR==1{next} /^## /{exit} {print}' \
    | grep -q '^\*\*Session start (R33):\*\*'
}

# --- Gate: the real session.md must carry the marker in its current round ---
if ! has_marker "$SESSION"; then
  echo "SESSION-START-CHECK: FAIL - the current round section of $SESSION has no '**Session start (R33):**' marker (the mandatory read protocol was not recorded - rules.md R33)" >&2
  exit 1
fi

# --- Negative control (R25): prove the detector discriminates ---
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

printf '%s\n' \
  '# Session Log — negative control' \
  '' \
  '## Round 2026-01-01 (test round)' \
  '' \
  'A round section WITHOUT the R33 marker.' \
  > "$tmp/nomarker.md"

printf '%s\n' \
  '# Session Log — negative control' \
  '' \
  '## Round 2026-01-01 (test round)' \
  '' \
  '**Session start (R33):** 2026-01-01 (branch test). Read: AGENTS.md.' \
  > "$tmp/marker.md"

if has_marker "$tmp/nomarker.md"; then
  echo "SESSION-START-CHECK: FAIL - negative control: marker-less round ACCEPTED (detector is blind)" >&2
  exit 1
fi
if ! has_marker "$tmp/marker.md"; then
  echo "SESSION-START-CHECK: FAIL - negative control: marker round REJECTED (detector is broken)" >&2
  exit 1
fi
echo "SESSION-START-CHECK: ok (current round carries the R33 marker, negative control proven)"
