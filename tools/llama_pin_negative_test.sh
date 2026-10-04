#!/usr/bin/env bash
# Negative control for tools/llama_pin_check.sh.
#
# WHY THIS EXISTS
# ---------------
# The pin gate was written from scratch in the same change that introduced the
# submodule. A brand new gate has never been seen to fail, so under rules.md R25
# it is not yet a gate - it is a hope. This script is the proof, and it lives in
# the repository and runs in CI so the proof cannot rot.
#
# The pin gate is also the only gate in this repository whose subject matter is
# git plumbing rather than source text: it asserts what a FRESH CLONE would
# obtain. That makes it unusually easy to write something that passes locally
# (the submodule happens to be checked out) while asserting nothing about a
# clone (nothing recorded in the index). Each scenario therefore attacks one of
# the three git states the gate reads, not a line of text.
#
# A NOTE ON THE FIXTURE
# ---------------------
# The pin is a real commit SHA, so a fixture cannot invent one: git will not
# produce a commit that hashes to 7fe450e1..., and it refuses a cache entry
# whose SHA is all zeroes. An earlier draft of this script tried, and every
# scenario failed for reasons that had nothing to do with the gate - which is
# exactly the trap a negative control must avoid, because a control that cannot
# even reach the gate proves nothing.
#
# So the fixture clones the REAL submodule (objects shared, not copied) and
# checks out the real pin. The "off the pin" commit is the pin's real parent.
# The commit is read out of the gate script itself, so this control cannot
# silently drift away from the gate when the pin is bumped.
#
# It never touches the repository: every scenario builds a throwaway
# super-project with its own .git, its own index, and its own submodule
# checkout, then runs the gate there. The gate derives its root from its own
# location, so a copied tree is a self-contained world.
#
# Usage: bash tools/llama_pin_negative_test.sh
# Exit 0 means every check was proved capable of failing.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GATE="$ROOT/tools/llama_pin_check.sh"
SUB="android/app/src/main/cpp/llama"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }

[ -f "$GATE" ] || { echo "missing $GATE"; exit 1; }

# The pin and the expected number of checks come from the gate under test, so
# this script cannot be updated to match a weakened gate without also failing.
PIN_COMMIT="$(sed -n 's/^PIN_COMMIT="\(.*\)"$/\1/p' "$GATE")"
PIN_TAG="$(sed -n 's/^PIN_TAG="\(.*\)"$/\1/p' "$GATE")"
EXPECTED_CHECKS="$(grep -c '^  *ok "' "$GATE")"

if [ -z "$PIN_COMMIT" ] || [ -z "$PIN_TAG" ]; then
  echo "could not read PIN_COMMIT/PIN_TAG out of $GATE" >&2
  exit 1
fi

# A real commit that is NOT the pin, used as the "somebody moved it" commit.
# The pin's own parent is the natural choice; a shallow clone may not have it,
# so fall back to any other commit the checkout knows about. Failing to find one
# at all means the fixture could not be built and the scenarios would be
# vacuous, which is reported as a failure rather than skipped.
OFF_PIN="$(git -C "$ROOT/$SUB" rev-parse --verify -q "${PIN_COMMIT}^" 2>/dev/null)"
if [ -z "$OFF_PIN" ] || [ "$OFF_PIN" = "$PIN_COMMIT" ]; then
  OFF_PIN="$(git -C "$ROOT/$SUB" rev-list --all 2>/dev/null \
             | grep -vxF "$PIN_COMMIT" | head -1)"
fi
if [ -z "$OFF_PIN" ]; then
  echo "no commit other than the pin ${PIN_COMMIT} is reachable in $ROOT/$SUB;" >&2
  echo "this control cannot build a fixture without one" >&2
  exit 1
fi

# Build a repository the gate should accept, so each scenario can then remove
# exactly one property and require exactly one check to turn red.
stage() {
  W="$(mktemp -d)"
  mkdir -p "$W/tools" "$W/android/app/src/main/cpp"
  cp "$GATE" "$W/tools/llama_pin_check.sh"
  printf '[submodule "llama"]\n\tpath = %s\n\turl = https://github.com/ggml-org/llama.cpp.git\n' \
    "$SUB" > "$W/.gitmodules"

  # A real checkout of the real pin, sharing objects instead of copying them.
  if ! git clone -q --shared --no-checkout "$ROOT/$SUB" "$W/$SUB" 2>/dev/null; then
    rm -rf "$W"
    bad "fixture: a real checkout of $PIN_TAG can be built" \
        "git clone --shared of $ROOT/$SUB failed; the scenarios would be vacuous"
    return 1
  fi
  if ! git -C "$W/$SUB" checkout -q "$PIN_COMMIT" 2>/dev/null; then
    rm -rf "$W"
    bad "fixture: a real checkout of $PIN_TAG can be built" \
        "cannot check out $PIN_COMMIT inside the clone"
    return 1
  fi

  git -C "$W" init -q .
  git -C "$W" config user.email negative-control@example.invalid
  git -C "$W" config user.name "negative control"
  git -C "$W" add .gitmodules
  # Record the gitlink in the index the way a real submodule checkout does, so
  # the baseline reads as "a fresh clone gets exactly this commit".
  git -C "$W" update-index --add --cacheinfo "160000,$PIN_COMMIT,$SUB"
  git -C "$W" commit -qm "pin llama.cpp"
  return 0
}

# expect <label> <check-name>
# The gate must exit non-zero AND the named check must be among the failures.
# Requiring the specific check, not just a red exit, is what stops a scenario
# from passing because an unrelated check happened to break.
expect() {
  label="$1"; check="$2"
  out="$(cd "$W" && bash tools/llama_pin_check.sh 2>&1)"; status=$?
  rm -rf "$W"
  if [ "$status" -eq 0 ]; then
    bad "$label" "the gate exited 0; a drifting pin would pass unnoticed"
  elif printf '%s' "$out" | grep -Fq "FAIL $check"; then
    ok "$label"
  else
    bad "$label" "the gate went red but not on the expected check '$check'"
    printf '%s\n' "$out" | grep -E '^(FAIL|LLAMA)' | sed 's/^/       /'
  fi
}

printf '== llama.cpp pin gate: negative control ==\n'
printf '   every scenario removes one property of the pin and requires the gate to go red\n\n'
printf '   pin: %s (%s)   off-pin commit used: %s\n\n' \
       "$PIN_COMMIT" "$PIN_TAG" "${OFF_PIN:0:12}"

# --- 0. the baseline must pass ----------------------------------------------
# A negative control that only ever sees red proves nothing if the control
# scenario cannot pass. If this goes red, the scenarios below are meaningless.
if stage; then
  out="$(cd "$W" && bash tools/llama_pin_check.sh 2>&1)"; status=$?
  rm -rf "$W"
  want="LLAMA-PIN-CHECK: $EXPECTED_CHECKS passed, 0 failed"
  if [ "$status" -eq 0 ] && printf '%s' "$out" | grep -Fq "$want"; then
    ok "the unmodified control scenario passes ($EXPECTED_CHECKS of $EXPECTED_CHECKS)"
  else
    bad "the unmodified control scenario passes ($EXPECTED_CHECKS of $EXPECTED_CHECKS)" \
        "exit $status, wanted: $want"
    printf '%s\n' "$out" | grep -E '^(FAIL|LLAMA)' | sed 's/^/       /'
  fi
fi

# --- 1. the submodule declaration -------------------------------------------
if stage; then
  grep -v 'submodule "llama"' "$W/.gitmodules" > "$W/.gitmodules.tmp" || true
  mv "$W/.gitmodules.tmp" "$W/.gitmodules"
  expect "a dropped .gitmodules entry is caught" \
         "llama.cpp is declared as a submodule"
fi

# --- 2. the recorded gitlink (what a fresh clone would get) ------------------
# The index is the only thing that survives into a clone, so this is the check
# that actually protects CI.
if stage; then
  git -C "$W" update-index --cacheinfo "160000,$OFF_PIN,$SUB"
  expect "a gitlink moved off the pin is caught" \
         "the recorded gitlink is $PIN_COMMIT ($PIN_TAG)"
fi

# --- 3. the working tree disagreeing with the index -------------------------
# The dangerous case in practice: the index is right, but somebody checked the
# submodule out to master and builds locally against sources CI never sees.
if stage; then
  git -C "$W/$SUB" checkout -q "$OFF_PIN"
  expect "a local checkout off the pin is caught" \
         "the checked-out submodule is at $PIN_COMMIT"
fi

# --- 4. committed GGUF weights ---------------------------------------------
if stage; then
  head -c 1024 /dev/zero > "$W/smollm2-135m-q4_k_m.gguf"
  git -C "$W" add smollm2-135m-q4_k_m.gguf
  git -C "$W" commit -qm "oops, committed weights"
  expect "a committed .gguf is caught" \
         "no GGUF weights are tracked"
fi

# --- 5. the vendored licence ------------------------------------------------
# A submodule is not covered by this repository's own licence file, so a missing
# or changed upstream licence is invisible to every other gate here.
if stage; then
  rm -f "$W/$SUB/LICENSE"
  expect "a missing upstream MIT licence is caught" \
         "the vendored llama.cpp carries its MIT licence"
fi

if stage; then
  printf 'GNU GENERAL PUBLIC LICENSE Version 3\n' > "$W/$SUB/LICENSE"
  expect "a non-MIT upstream licence is caught" \
         "the vendored llama.cpp carries its MIT licence"
fi

printf '\nLLAMA-PIN-NEGATIVE: %d proved, %d could not fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
