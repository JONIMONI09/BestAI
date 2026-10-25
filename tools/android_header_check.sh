#!/usr/bin/env bash
# Android model-header diagnostics gate.
#
# The Android import rejects a model before the engine ever sees it. The
# rules it applies must match the C loader AND the web console's
# /api/models/inspect report, and the refusal must name what the user
# actually picked ("this is a GGUF file…") instead of "wrong magic".
#
# There is no device or emulator in CI (see docs/gpu-feasibility.md,
# "Measured on device"), so this is a static gate over the Kotlin source:
# every check below fails when the corresponding behaviour is removed.
# The negative control is the script itself, pointed at a copy with a rule
# deleted:
#
#   bash tools/android_header_check.sh /tmp/broken   # must exit 1
set -uo pipefail

SRC="${1:-android/app/src/main/java/dev/hydrastone}"
STRINGS="android/app/src/main/res/values/strings.xml"

fail=0
checks=0

check() { # check <description> <file> <fixed-string>
  local what="$1" file="$2" needle="$3"
  checks=$((checks + 1))
  if [ ! -f "$file" ]; then
    echo "FAIL: $what ($file not found)"
    fail=1
    return
  fi
  if grep -qF -- "$needle" "$file"; then
    echo "ok   $what"
  else
    echo "FAIL: $what - '$needle' not found in $file"
    fail=1
  fi
}

MAIN="$SRC/MainActivity.kt"

# --- the analysis exists and reports every rule -------------------------
check "the import runs a full header analysis" "$MAIN" "analyseHydraFile(tmp, bytes)"
check "every rule is logged, not only the first" "$MAIN" "report.rules.filter { !it.ok }"
check "the raw header is logged for the user" "$MAIN" "R.string.log_import_header"
check "the report carries a pass/fail flag" "$MAIN" "val valid: Boolean get() = rules.all { it.ok }"

# --- the rules match the server's rule ids -----------------------------
for rule in magic version dim vocab layers weights_offset weights_fit \
            pairs_covered shape_within_format_ceiling; do
  check "rule '$rule' is implemented" "$MAIN" "\"$rule\""
done

# --- naming the format instead of a hex value --------------------------
check "GGUF is detected" "$MAIN" "GGUF_MAGIC"
check "the GGUF message exists" "$STRINGS" "err_magic_gguf"
check "the GGUF message names .hydra" "$STRINGS" "The engine reads .hydra files"
check "other formats are named too" "$MAIN" "ZIP_MAGIC"
check "an unknown magic still states both values" "$STRINGS" "err_magic_unknown"

# --- 64-bit arithmetic, not Int ----------------------------------------
check "weights end is summed in Long" "$MAIN" "val weightsEnd = offset + len"
reject_int_sum() {
  local what="$1" file="$2"
  checks=$((checks + 1))
  if grep -qF -- "$3" "$file"; then
    echo "FAIL: $what - found '$3'"
    fail=1
  else
    echo "ok   $what"
  fi
}
reject_int_sum "no Int-based offset+len sum" "$MAIN" "val weightsEnd = (offset + len).toInt()"

# --- the import limit is stated in human units -------------------------
check "the size limit is formatted for humans" "$MAIN" "formatBytes(MAX_UPLOAD_BYTES)"
check "the size rule has a message" "$STRINGS" "err_too_large_human"

if [ "$fail" != "0" ]; then
  echo "ANDROID-HEADER-CHECK: FAILED ($checks checks)"
  exit 1
fi
echo "ANDROID-HEADER-CHECK: $checks checks passed"