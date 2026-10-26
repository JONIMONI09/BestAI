#!/usr/bin/env bash
# Cross-surface sampler contract gate.
#
# The accept/reject rules for a sampling run must be the SAME on every
# surface that exposes one:
#   1. the CLI      (src/main.c: --temp/--min-p/--rep-penalty/--rep-window)
#   2. the Android  (android/app/src/main/cpp/hydra_sample_cfg.h, used by
#                    hydra_jni.c runInferenceSampled)
#   3. the web      (server.js /api/infer)
# A surface that silently re-tunes a range (say, allowing min-p 1.5)
# would let the same request run differently depending on where it was
# typed - a rule that lives in two places is a bug (AGENTS.md 2.5, R12).
#
# This gate is static (no engine run): it pins the constants, the check
# order, and the pass-through on each surface, and it COMPUTES the
# numeric agreement between the C header and server.js instead of
# trusting two separately pinned literals. The deliberate difference is
# pinned too: the CLI's --temp is finite-only (power users type what they
# mean), while the UI-facing surfaces (the JNI header + server.js) cap
# temperature at 20.
#
# Negative control is the script itself pointed at a broken tree:
#   bash tools/sampler_contract_check.sh /tmp/broken   # must exit 1
set -uo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
JNI_H="$ROOT/android/app/src/main/cpp/hydra_sample_cfg.h"
JNI_C="$ROOT/android/app/src/main/cpp/hydra_jni.c"
CLI="$ROOT/src/main.c"
WEB="$ROOT/server.js"

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

reject() { # reject <description> <file> <fixed-string-that-must-not-exist>
  local what="$1" file="$2" needle="$3"
  checks=$((checks + 1))
  if [ -f "$file" ] && grep -qF -- "$needle" "$file"; then
    echo "FAIL: $what - found '$needle' in $file"
    fail=1
  else
    echo "ok   $what"
  fi
}

# ---- the Android surface: the header is the single source --------------
check "JNI header pins the temperature range [0, 20]" "$JNI_H" "#define HYDRA_SAMPLE_TEMP_MIN   0.0f"
check "JNI header pins TEMP_MAX = 20" "$JNI_H" "#define HYDRA_SAMPLE_TEMP_MAX   20.0f"
check "JNI header pins the min-p range [0, 1]" "$JNI_H" "#define HYDRA_SAMPLE_MINP_MIN   0.0f"
check "JNI header pins MINP_MAX = 1" "$JNI_H" "#define HYDRA_SAMPLE_MINP_MAX   1.0f"
check "JNI header pins the penalty floor 1.0" "$JNI_H" "#define HYDRA_SAMPLE_REP_MIN    1.0f"
check "JNI header pins the greedy default temp 0" "$JNI_H" "#define HYDRA_SAMPLE_DEFAULT_TEMP 0.0f"
check "JNI header pins the greedy default min-p 0" "$JNI_H" "#define HYDRA_SAMPLE_DEFAULT_MINP 0.0f"
check "JNI header pins the greedy default penalty 1" "$JNI_H" "#define HYDRA_SAMPLE_DEFAULT_REP  1.0f"

# Check order is part of the contract: the error a multi-bad request
# receives is the FIRST offender in the order temp, min-p, penalty.
l1=$(grep -n 'return HYDRA_SAMPLE_BAD_TEMP' "$JNI_H" | head -1 | cut -d: -f1)
l2=$(grep -n 'return HYDRA_SAMPLE_BAD_MINP' "$JNI_H" | head -1 | cut -d: -f1)
l3=$(grep -n 'return HYDRA_SAMPLE_BAD_REP' "$JNI_H" | head -1 | cut -d: -f1)
checks=$((checks + 1))
if [ -n "${l1:-}" ] && [ -n "${l2:-}" ] && [ -n "${l3:-}" ] && [ "$l1" -lt "$l2" ] && [ "$l2" -lt "$l3" ]; then
  echo "ok   check order is temperature, min-p, repetition penalty"
else
  echo "FAIL: check order drifted (lines ${l1:-?} ${l2:-?} ${l3:-?})"
  fail=1
fi

# The bridge must USE the header, not carry a second copy of the rules.
check "hydra_jni.c includes the shared header" "$JNI_C" '#include "hydra_sample_cfg.h"'
check "hydra_jni.c calls the shared validation" "$JNI_C" "hydra_sample_cfg_validate(temp, mp, rp)"
reject "no inline 'temp > 20' literal left in hydra_jni.c" "$JNI_C" "temp > 20.0f"

# ---- the CLI surface ----------------------------------------------------
check "CLI pins the greedy default temp 0" "$CLI" "sampler.temperature = 0.0f;"
check "CLI pins the greedy default min-p 0" "$CLI" "sampler.min_p = 0.0f;"
check "CLI pins the greedy default penalty 1" "$CLI" "sampler.repetition_penalty = 1.0f;"
check "CLI pins the greedy default window 64" "$CLI" "sampler.penalty_window = 64;"
check "CLI pins the min-p range [0, 1]" "$CLI" "if (v < 0.0 || v > 1.0) {"
check "CLI pins the penalty floor 1.0" "$CLI" "if (v < 1.0) {"
check "CLI exposes --temp" "$CLI" '"--temp"'
check "CLI exposes --min-p" "$CLI" '"--min-p"'
check "CLI exposes --rep-penalty" "$CLI" '"--rep-penalty"'
check "CLI exposes --rep-window" "$CLI" '"--rep-window"'

# ---- the web surface ----------------------------------------------------
check "server.js pins SAMPLER_TEMP_MAX" "$WEB" "const SAMPLER_TEMP_MAX = 20;"
check "server.js pins SAMPLER_MINP_MAX" "$WEB" "const SAMPLER_MINP_MAX = 1;"
check "server.js pins SAMPLER_REP_MIN" "$WEB" "const SAMPLER_REP_MIN = 1;"
check "server.js pins SAMPLER_REP_WINDOW_MAX" "$WEB" "const SAMPLER_REP_WINDOW_MAX = 4096;"
check "server.js rejects non-number values (typeof, not Number())" "$WEB" "typeof v !== 'number' || !Number.isFinite(v)"
check "server.js passes --temp only when sent" "$WEB" "if (sampler && sampler.temperature !== undefined) args.push('--temp'"
check "server.js passes --min-p only when sent" "$WEB" "if (sampler && sampler.minP !== undefined) args.push('--min-p'"
check "server.js passes --rep-penalty only when sent" "$WEB" "if (sampler && sampler.repetitionPenalty !== undefined) args.push('--rep-penalty'"
check "server.js passes --rep-window only when sent" "$WEB" "if (sampler && sampler.repWindow !== undefined) args.push('--rep-window'"

# ---- computed agreement: C header vs server.js --------------------------
# Two separately pinned literals can both be present yet disagree; the
# gate therefore EXTRACTS both and compares them.
c_temp=$(sed -n 's/.*HYDRA_SAMPLE_TEMP_MAX[[:space:]]*\([0-9.]*\)f.*/\1/p' "$JNI_H" 2>/dev/null | head -1)
js_temp=$(sed -n 's/.*SAMPLER_TEMP_MAX = \([0-9.]*\);.*/\1/p' "$WEB" 2>/dev/null | head -1)
checks=$((checks + 1))
if [ -n "${c_temp:-}" ] && [ -n "${js_temp:-}" ] && awk -v a="${c_temp%.*}" -v b="${js_temp%.*}" 'BEGIN { exit (a == b ? 0 : 1) }'; then
  echo "ok   temperature cap agrees: C header $c_temp == server.js $js_temp"
else
  echo "FAIL: temperature cap drift: C header '${c_temp:-?}' vs server.js '${js_temp:-?}'"
  fail=1
fi
c_minp=$(sed -n 's/.*HYDRA_SAMPLE_MINP_MAX[[:space:]]*\([0-9.]*\)f.*/\1/p' "$JNI_H" 2>/dev/null | head -1)
js_minp=$(sed -n 's/.*SAMPLER_MINP_MAX = \([0-9.]*\);.*/\1/p' "$WEB" 2>/dev/null | head -1)
checks=$((checks + 1))
if [ -n "${c_minp:-}" ] && [ -n "${js_minp:-}" ] && [ "${c_minp%.*}" = "${js_minp%.*}" ]; then
  echo "ok   min-p ceiling agrees: C header $c_minp == server.js $js_minp"
else
  echo "FAIL: min-p ceiling drift: C header '${c_minp:-?}' vs server.js '${js_minp:-?}'"
  fail=1
fi
c_rep=$(sed -n 's/.*HYDRA_SAMPLE_REP_MIN[[:space:]]*\([0-9.]*\)f.*/\1/p' "$JNI_H" 2>/dev/null | head -1)
js_rep=$(sed -n 's/.*SAMPLER_REP_MIN = \([0-9.]*\);.*/\1/p' "$WEB" 2>/dev/null | head -1)
checks=$((checks + 1))
if [ -n "${c_rep:-}" ] && [ -n "${js_rep:-}" ] && [ "${c_rep%.*}" = "${js_rep%.*}" ]; then
  echo "ok   penalty floor agrees: C header $c_rep == server.js $js_rep"
else
  echo "FAIL: penalty floor drift: C header '${c_rep:-?}' vs server.js '${js_rep:-?}'"
  fail=1
fi

if [ "$fail" != "0" ]; then
  echo "SAMPLER-CONTRACT-CHECK: FAILED ($checks checks)"
  exit 1
fi
echo "SAMPLER-CONTRACT-CHECK: $checks checks passed"
