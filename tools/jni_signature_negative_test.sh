#!/usr/bin/env bash
# Negative control for tools/jni_signature_check.sh.
#
# WHY THIS EXISTS
# ---------------
# A signature gate that cannot fail is decoration. It parses the Kotlin
# `external fun` declarations, regenerates the JNI header with javac, and
# compares every symbol + parameter list against the C bridge - so the
# thing that must be proved is that it goes RED when the contract is
# broken, in BOTH directions:
#   1. the Kotlin side declares a method the C bridge does not implement
#      (a phantom native method - the classic "the Kotlin side moved on"
#      drift),
#   2. an existing method's parameter list no longer matches the C side
#      (the symbol exists, the descriptor does not - a stricter failure
#      the symbol-existence check alone would miss).
#
# .agents/rules.md R25 requires a gate to fail once before it can be
# trusted. This script is that proof, and it runs in CI so the proof
# cannot rot.
#
# It never touches the repository: each scenario copies the bridge pair
# into a temporary tree and mutates the copy. The gate takes the file
# pair as arguments (that is exactly what the optional parameters are
# for), so a staged copy is a self-contained world.
#
# Usage: bash tools/jni_signature_negative_test.sh
# Exit 0 means both scenarios were proved capable of failing.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GATE="$ROOT/tools/jni_signature_check.sh"
KT="$ROOT/android/app/src/main/java/dev/hydrastone/LlamaBridge.kt"
CPP="$ROOT/android/app/src/main/cpp/llama_jni.cpp"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; return 0; }

[ -f "$GATE" ] || { echo "missing $GATE"; exit 1; }
[ -f "$KT" ]   || { echo "missing $KT"; exit 1; }
[ -f "$CPP" ]  || { echo "missing $CPP"; exit 1; }

if ! command -v javac > /dev/null; then
  # The gate itself SKIPs without javac ("SKIP: javac not available"),
  # so its failure behaviour is unprovable on this machine - say so
  # instead of pretending the proof ran.
  echo "SKIP: javac not available (the gate itself SKIPs without it; the proof runs in CI)"
  exit 0
fi

stage() {
  W="$(mktemp -d)"
  mkdir -p "$W/java" "$W/cpp"
  cp "$KT" "$W/java/LlamaBridge.kt"
  cp "$CPP" "$W/cpp/llama_jni.cpp"
  echo "$W"
}

# --- scenario 1: a native method the C bridge does not implement ---------
W="$(stage)"
cat >> "$W/java/LlamaBridge.kt" <<'KT_EOF'

    /* NEGATIVE-CONTROL PHANTOM: no C implementation exists for this. */
    external fun phantomMethod(x: Int): Int
KT_EOF
if bash "$GATE" "$W/java/LlamaBridge.kt" "$W/cpp/llama_jni.cpp" > /dev/null 2>&1; then
  bad "phantom native method must be rejected" "the gate passed a bridge that declares phantomMethod"
else
  ok "phantom native method is rejected (gate exits non-zero)"
fi
rm -rf "$W"

# --- scenario 2: the parameter list of an existing method drifts ---------
# load(path: String, nCtx: Int) -> load(path: String): the C symbol
# Java_dev_hydrastone_LlamaBridge_load still exists, but the descriptor
# javac derives no longer matches the C parameter list.
W="$(stage)"
sed -i 's/external fun load(path: String, nCtx: Int): String/external fun load(path: String): String/' "$W/java/LlamaBridge.kt"
if grep -qF 'external fun load(path: String): String' "$W/java/LlamaBridge.kt"; then
  if bash "$GATE" "$W/java/LlamaBridge.kt" "$W/cpp/llama_jni.cpp" > /dev/null 2>&1; then
    bad "drifted parameter list must be rejected" "the gate passed load(path) while the C side is load(jstring, jint)"
  else
    ok "drifted parameter list is rejected (gate exits non-zero)"
  fi
else
  bad "could not stage scenario 2" "the load() line in LlamaBridge.kt changed; update this test"
fi
rm -rf "$W"

if [ "$FAIL" != "0" ]; then
  echo "JNI-SIGNATURE-NEGATIVE: FAILED ($PASS ok, $FAIL failed)"
  exit 1
fi
echo "JNI-SIGNATURE-NEGATIVE: $PASS scenarios proved capable of failing"
