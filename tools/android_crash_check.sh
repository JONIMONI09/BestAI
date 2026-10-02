#!/usr/bin/env bash
# Android crash-reporting and cancel gate.
#
# The behaviour the task asks for cannot be executed here: there is no
# device, no emulator and no adb in this container (see
# docs/gpu-feasibility.md, "Measured on device"). What CAN be checked
# without a device is the set of properties that were the actual bugs:
#
#   1. the UncaughtExceptionHandler writes the report to a file BEFORE it
#      delegates to the previous handler, and never starts an Activity,
#   2. the previous handler is always called (otherwise the platform never
#      learns about the crash),
#   3. the stored report is surfaced on the next launch, not during the
#      crash,
#   4. the report screen offers copy AND delete,
#   5. the JNI batching loop is the shared, host-tested hydra_run_steps()
#      and not a second copy of the same policy,
#   6. steps <= 0 is rejected before the engine is loaded,
#   7. the token-only fast path is documented as leaving state[1..dim-1]
#      stale, so no UI may render a full state strip from it.
#
# These are greps, and greps are weak - which is exactly why each one is
# written to FAIL when the property is removed. The optional argument
# points the script at a different Kotlin tree for the negative control:
#
#   bash tools/android_crash_check.sh /tmp/broken   # must exit 1
set -uo pipefail

SRC="${1:-android/app/src/main/java/dev/hydrastone}"
CPP="android/app/src/main/cpp"
ENGINE_H="include/hydra_model.h"

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

reject() { # reject <description> <file> <fixed-string>
  local what="$1" file="$2" needle="$3"
  checks=$((checks + 1))
  if [ ! -f "$file" ]; then
    echo "FAIL: $what ($file not found)"
    fail=1
    return
  fi
  if grep -qF -- "$needle" "$file"; then
    echo "FAIL: $what - '$needle' must NOT appear in $file"
    fail=1
  else
    echo "ok   $what"
  fi
}

HANDLER="$SRC/CrashHandler.kt"
MAIN="$SRC/MainActivity.kt"
ACTIVITY="$SRC/CrashActivity.kt"
BRIDGE="$SRC/HydraBridge.kt"

# --- 1+2: the handler contract -------------------------------------------
reject "the crash handler never starts an Activity" \
  "$HANDLER" "startActivity"
reject "the crash handler holds no Activity intent" \
  "$HANDLER" "Intent("
check "the report is written to a file in filesDir" \
  "$HANDLER" "writeReport(dir, report)"
check "the previous handler is always invoked" \
  "$HANDLER" "previous?.uncaughtException(thread, error)"
check "the write happens before the delegation" \
  "$HANDLER" "WRITE the report"

# Order matters, so it is checked as an order, not as two greps: a handler
# that delegates first and writes afterwards satisfies both greps and still
# loses the report.
if [ -f "$HANDLER" ]; then
  checks=$((checks + 1))
  write_line="$(grep -n "writeReport(dir, report)" "$HANDLER" | head -1 | cut -d: -f1)"
  delegate_line="$(grep -n "previous?.uncaughtException(thread, error)" "$HANDLER" | head -1 | cut -d: -f1)"
  if [ -n "$write_line" ] && [ -n "$delegate_line" ] && [ "$write_line" -lt "$delegate_line" ]; then
    echo "ok   the report is written before the crash is delegated"
  else
    echo "FAIL: the write (line ${write_line:-none}) must come before the delegation (line ${delegate_line:-none})"
    fail=1
  fi
fi

check "the stored report is readable" "$HANDLER" "fun pendingReport"
check "the stored report can be deleted" "$HANDLER" "fun clearReport"

# --- 3+4: the report surface --------------------------------------------
check "MainActivity looks for a stored report on start" \
  "$MAIN" "CrashHandler.pendingReport(filesDir)"
check "MainActivity opens the report screen" \
  "$MAIN" "CrashActivity::class.java"
check "the report screen offers a copy button" \
  "$ACTIVITY" "crash-copy-button"
check "the report screen offers a delete action" \
  "$ACTIVITY" "crash-delete-button"
check "deleting a report is possible" \
  "$ACTIVITY" "CrashHandler.clearReport(filesDir)"

# --- 5+6: the JNI loop --------------------------------------------------
check "the JNI loop is the shared, host-tested policy" \
  "$CPP/hydra_jni.c" "hydra_run_steps(&batch"
check "the shared policy exists as a header" \
  "$CPP/hydra_batch.h" "hydra_run_steps"
check "a host test drives that same header" \
  "tests/test_jni_batch.c" "hydra_batch.h"
check "steps below 1 are rejected" \
  "$CPP/hydra_jni.c" "steps must be >= 1"
reject "steps are no longer silently clamped to 1" \
  "$CPP/hydra_jni.c" "if (steps < 1) steps = 1;"

# --- the cooperative cancel --------------------------------------------
check "the bridge declares cancel()" "$BRIDGE" "external fun cancel()"
check "cancel sets the native flag" \
  "$CPP/hydra_jni.c" "Java_dev_hydrastone_HydraBridge_cancel"
check "the flag is checked in the step loop" \
  "$CPP/hydra_jni.c" "if (g_cancel_requested) return -1;"
check "a cancelled run is not reported as an engine failure" \
  "$CPP/hydra_jni.c" "\"cancelled\":true"
check "the UI says cancelled, not failed" "$MAIN" "R.string.log_cancelled"

# --- 7: token-only state semantics -------------------------------------
check "the fast path documents that only dimension 0 is updated" \
  "$ENGINE_H" "state[0]"
check "the JNI path documents that a token-only switch would stale the rest" \
  "$CPP/hydra_jni.c" "state[1..dim-1]"

if [ "$fail" != "0" ]; then
  echo "ANDROID-CRASH-CHECK: FAILED ($checks checks)"
  exit 1
fi
echo "ANDROID-CRASH-CHECK: $checks checks passed"