#!/usr/bin/env bash
# Gate: no Compose scrollable container may nest inside another scrollable
# of the same axis (the "infinity maximum constraints" crash class).
#
# WHY
# ---
# A vertically scrollable node (Modifier.verticalScroll, or a LazyColumn)
# that is measured with an infinite max height makes Compose throw
# IllegalStateException at measure time - on every device, the moment the
# tree is composed. That is exactly the 2026-10-26 Hydra-Stone crash
# (android 15 / arm64-v8a): ChatTab's header lambda was a
# Column(Modifier.verticalScroll) while AdaptiveHeaderSplit invoked the
# lambda inside ITS OWN scrolling column.
#
# An instrumented Compose UI test would be the ideal detector, but CI
# builds the APK without an emulator, so the detector is static (see
# tools/compose_scroll_guard.py: detector 1 = lexical nesting, detector 2
# = a composable lambda invoked inside a scrollable whose definition
# carries scrollable content). This wrapper is the GATE around it and
# carries the controls (.agents/rules.md R25/R26):
#
#   1. the real Android sources must come back clean,
#   2. the pre-fix crash shape from git history (d0c7679 HydraApp.kt -
#      the exact tree that produced the crash report) MUST be detected,
#   3. the bad lexical fixture (4 illegal patterns) must yield 4 findings,
#   4. the bad lambda fixture (the crash shape, standalone) must be
#      detected, and only the header pair,
#   5. the ok fixtures (legal layouts, incl. the FIXED app shape) must
#      yield 0 findings - the guard may not reject working layouts.
#
# If the guard starts missing nested scrollables, steps 2/3/4 fail the
# build HERE - not a phone. If it starts over-detecting legal layouts,
# step 5 fails the build here, not a working app.
#
# Usage: bash tools/compose_scroll_check.sh
# Exit 0 when every step behaves as asserted, 1 otherwise.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUARD="$ROOT/tools/compose_scroll_guard.py"
LEX_BAD="$ROOT/tests/fixtures/compose/scroll_nested_bad.kt"
LAM_BAD="$ROOT/tests/fixtures/compose/scroll_lambda_bad.kt"
OK_FIXTURES=(
  "$ROOT/tests/fixtures/compose/scroll_ok.kt"
  "$ROOT/tests/fixtures/compose/scroll_lambda_ok.kt"
)

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }

cd "$ROOT" || exit 1

# Collect the real Compose sources (app module, every source set).
mapfile -t SOURCES < <(find android/app/src -name '*.kt' | sort)
if [ "${#SOURCES[@]}" -eq 0 ]; then
  bad "Android Kotlin sources found" "no .kt files under android/app/src"
  exit 1
fi
ok "Android Kotlin sources found (${#SOURCES[@]} files)"

# count only finding lines ("FAIL <file>:<line>: ..."); the guard's final
# "FAIL N nested scrollable container(s) found" summary must not count
findings_of() {
  printf '%s\n' "$1" | grep -cE '^FAIL [^ ]+:[0-9]+:' || true
}

# 1. the real sources are clean.
OUT="$(python3 "$GUARD" "${SOURCES[@]}" 2>&1)"
RC=$?
if [ "$RC" -eq 0 ]; then
  ok "real Android sources contain no nested scrollables (crash class blocked)"
else
  bad "real Android sources contain no nested scrollables" "$OUT"
fi

# 2. the pre-fix crash shape is still detected (regression proof).
#    d0c7679 is the merged PR #51 tip - the tree that produced the crash
#    report. Step 4 below carries the same proof standalone, so a shallow
#    clone without this commit still keeps the negative control.
PRE_FIX="$(mktemp /tmp/hydra-pre-fix-XXXX.kt)"
if git -C "$ROOT" show d0c7679:android/app/src/main/java/dev/hydrastone/ui/HydraApp.kt > "$PRE_FIX" 2>/dev/null; then
  OUT="$(python3 "$GUARD" "$PRE_FIX" 2>&1)"
  RC=$?
  N=$(findings_of "$OUT")
  if [ "$RC" -ne 0 ] && [ "$N" -ge 1 ]; then
    ok "pre-fix crash shape (d0c7679 HydraApp.kt) is detected ($N finding(s))"
  else
    bad "pre-fix crash shape (d0c7679 HydraApp.kt) is detected" \
        "rc=$RC, findings=$N - the guard let the exact 2026-10-26 crash layout pass"
  fi
else
  ok "pre-fix crash shape check skipped (shallow clone); standalone proof is step 4"
fi
rm -f "$PRE_FIX"

# 3. the bad lexical fixture: every illegal pattern detected (4 findings).
OUT="$(python3 "$GUARD" "$LEX_BAD" 2>&1)"
RC=$?
N=$(findings_of "$OUT")
if [ "$RC" -ne 0 ] && [ "$N" -ge 4 ]; then
  ok "bad lexical fixture yields $N findings (>= 4 expected)"
else
  bad "bad lexical fixture yields 4 findings" "rc=$RC, findings=$N"
fi

# 4. the bad lambda fixture: the crash shape across the lambda boundary.
#    Exactly the header pair is illegal here; the body's LazyColumn sits
#    in a non-scrollable column and must stay silent.
OUT="$(python3 "$GUARD" "$LAM_BAD" 2>&1)"
RC=$?
N=$(findings_of "$OUT")
if [ "$RC" -ne 0 ] && [ "$N" -ge 1 ] && printf '%s\n' "$OUT" | grep -q "header()"; then
  ok "bad lambda fixture is detected and names the 'header()' pair"
else
  bad "bad lambda fixture is detected" "rc=$RC, findings=$N"
  [ -n "$OUT" ] && printf '%s\n' "$OUT" | sed 's/^/     /'
fi

# 5. the ok fixtures: legal layouts (incl. the FIXED app shape) stay legal.
for F in "${OK_FIXTURES[@]}"; do
  OUT="$(python3 "$GUARD" "$F" 2>&1)"
  RC=$?
  if [ "$RC" -eq 0 ]; then
    ok "$(basename "$F") yields 0 findings"
  else
    bad "$(basename "$F") yields 0 findings" "$OUT"
  fi
done

echo
if [ "$FAIL" -eq 0 ]; then
  echo "compose_scroll_check: all $PASS checks passed"
  exit 0
fi
echo "compose_scroll_check: $FAIL check(s) FAILED"
exit 1
