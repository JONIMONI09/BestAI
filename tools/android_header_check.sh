#!/usr/bin/env bash
# Android model-header diagnostics gate.
#
# The Android import rejects a model before the engine ever sees it. The
# rules it applies must match the C loader AND the web console's
# /api/models/inspect report, and the refusal must name what the user
# actually picked ("this is a GGUF file…") instead of "wrong magic".
#
# There is no device or emulator in CI (see .agents/docs/gpu-feasibility.md,
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

# --- the v2 header path, verified alongside the v1 header path ----------
# A .hydra2 container ("HYDRA2\0\0", include/hydra_v2_container.h
# HYDRA2_MAGIC_STR) starts with the SAME four "HYDR" bytes as a v1
# model, so the v1 magic rule alone misreports it ("magic matches
# .hydra" + a generic version error). The Kotlin side must carry the
# discriminator (bytes 4..7 as a little-endian u32) and name the format.
# The constant is RE-DERIVED from the C header, so a format change in
# include/hydra_v2_container.h turns this gate red instead of rotting
# the Kotlin constant silently (the same cross-surface discipline the
# C-loader rule ids above pin against MainActivity.kt).
C_V2_HDR="$(cd "$(dirname "$0")/.." && pwd)/include/hydra_v2_container.h"
check "the v2 tail constant exists in MainActivity.kt" "$MAIN" "HYDRA2_TAIL_U32"
check "the v2 naming rule exists" "$MAIN" 'HeaderRule("format", false, getString(R.string.err_magic_hydra2))'
check "the v2 message exists" "$STRINGS" "err_magic_hydra2"
check "the v2 message names .hydra2" "$STRINGS" ".hydra2 container"
if command -v python3 > /dev/null; then
  expected_v2_tail=$(python3 - "$C_V2_HDR" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r'#define\s+HYDRA2_MAGIC_STR\s*"((?:[^"\\]|\\.)*)"', src)
if not m:
    sys.exit("HYDRA2_MAGIC_STR not found")
raw = m.group(1).encode("utf-8").decode("unicode_escape").encode("latin-1")
print("0x%08XL" % int.from_bytes(raw[4:8], "little"))
PY
  )
  if [ -n "${expected_v2_tail:-}" ]; then
    check "the Kotlin v2-tail constant matches the C header ($expected_v2_tail)" "$MAIN" "const val HYDRA2_TAIL_U32 = $expected_v2_tail"
  else
    checks=$((checks + 1))
    echo "FAIL: could not derive the v2 tail from $C_V2_HDR"
    fail=1
  fi
  # The same discipline for the v1 header: the Kotlin HYDRA_MAGIC must
  # equal the little-endian u32 of the C header's magic bytes.
  expected_v1_magic=$(python3 - "$(cd "$(dirname "$0")/.." && pwd)/include/hydra_model.h" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r'#define\s+HYDRA_MAGIC\s+(0x[0-9a-fA-F]+)', src)
if not m:
    sys.exit("HYDRA_MAGIC not found")
val = int(m.group(1), 16)
print("0x%08XL" % val)
PY
  )
  if [ -n "${expected_v1_magic:-}" ]; then
    check "the Kotlin v1 magic matches the C header ($expected_v1_magic)" "$MAIN" "const val HYDRA_MAGIC = $expected_v1_magic"
  else
    checks=$((checks + 1))
    echo "FAIL: could not derive HYDRA_MAGIC from include/hydra_model.h"
    fail=1
  fi
fi

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