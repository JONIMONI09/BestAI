#!/usr/bin/env bash
# Negative control for tools/android_autostart_check.sh.
#
# WHY THIS EXISTS
# ---------------
# A lint gate that cannot fail is decoration. The Compose rewrite replaced
# nine of this gate's checks, because the mechanisms they asserted (CheckBox,
# setContentView(ScrollView), modelLabel.text, ...) no longer exist. Changing
# what a gate asserts is exactly when it is most likely to be rewritten into
# something that always passes, so every rewritten check is proved here by
# REMOVING the behaviour it is supposed to protect and asserting that the
# named check turns red.
#
# rules.md R25 requires a gate to fail once before it can be trusted. This
# script is that proof, and it runs in CI so the proof cannot rot.
#
# It never touches the repository: each scenario copies the sources into a
# temporary tree, mutates the copy, and runs the gate there. The gate derives
# its root from its own location, so a copied tree is a self-contained world.
#
# Usage: bash tools/android_autostart_negative_test.sh
# Exit 0 means every check was proved capable of failing.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GATE="$ROOT/tools/android_autostart_check.sh"

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }

[ -x "$GATE" ] || [ -f "$GATE" ] || { echo "missing $GATE"; exit 1; }

# Lay down a self-contained copy of everything the gate reads.
stage() {
  W="$(mktemp -d)"
  mkdir -p "$W/tools" \
           "$W/android/app/src/main/java/dev/hydrastone/ui" \
           "$W/android/app/src/main/res/values" \
           "$W/android/app/src/main/assets"
  cp "$GATE" "$W/tools/android_autostart_check.sh"
  cp "$ROOT/tools/make_model.py" "$W/tools/make_model.py" 2>/dev/null || true
  cp "$ROOT/android/app/src/main/java/dev/hydrastone/MainActivity.kt" \
     "$W/android/app/src/main/java/dev/hydrastone/MainActivity.kt"
  cp "$ROOT/android/app/src/main/java/dev/hydrastone/ui/HydraApp.kt" \
     "$W/android/app/src/main/java/dev/hydrastone/ui/HydraApp.kt"
  cp "$ROOT/android/app/src/main/res/values/strings.xml" \
     "$W/android/app/src/main/res/values/strings.xml"
  cp "$ROOT/android/app/src/main/assets/starter.hydra" \
     "$W/android/app/src/main/assets/starter.hydra"
}

# scenario <label> <check-name> <file-to-mutate> <python-mutation>
# The mutation is a python expression receiving (path, text) and returning the
# new text. It must REMOVE the behaviour under test.
scenario() {
  label="$1"; check="$2"; target="$3"; mutation="$4"
  stage
  python3 - "$W/$target" <<PY
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
s = $mutation
open(p, "w", encoding="utf-8").write(s)
PY
  out="$(cd "$W" && bash tools/android_autostart_check.sh 2>&1)"
  rm -rf "$W"
  if printf '%s' "$out" | grep -Fq "FAIL $check"; then
    ok "$label"
  else
    bad "$label" "the check '$check' still passed after its behaviour was removed"
    printf '%s\n' "$out" | grep -E '^(FAIL|ANDROID)' | sed 's/^/       /'
  fi
}

M=android/app/src/main/java/dev/hydrastone/MainActivity.kt
U=android/app/src/main/java/dev/hydrastone/ui/HydraApp.kt

printf '== Android auto-start gate: negative control ==\n'
printf '   every scenario removes a behaviour and requires the gate to go red\n\n'

# --- the checks this rewrite touched -------------------------------------

scenario "auto-run effect still runs a generation" \
  "the automatic run goes through the same path as the button" "$M" \
  's.replace("if (autoRun && ui.value.hasModel) startInference()", "if (autoRun && ui.value.hasModel) log(\"nothing\")")'

scenario "the auto-run switch still exists" \
  "the auto-run switch exists" "$U" \
  's.replace("Switch(checked = checked", "Text(\"\" + checked")'

scenario "the auto-run switch is still labelled from resources" \
  "the switch is labelled from resources, not hardcoded" "$U" \
  's.replace("stringResource(R.string.action_auto_run)", "\"Run automatically\"")'

scenario "the active model is still named on screen" \
  "the active model is shown in the UI" "$U" \
  's.replace("\"${stringResource(R.string.label_model)}: $modelLabel\"", "\"model ready\"")'

scenario "the model label is still built from a resource" \
  "the model label text comes from resources" "$U" \
  's.replace("stringResource(R.string.label_model)", "stringResource(R.string.app_name)")'

scenario "send is still disabled without a model" \
  "no model means the run button is disabled" "$U" \
  's.replace("enabled = state.hasModel && state.input.isNotBlank()", "enabled = true")'

scenario "the overflowing tabs still scroll" \
  "the whole screen is scrollable" "$U" \
  's.replace("verticalScroll(rememberScrollState())", "")'

scenario "the conversation is still a lazy list" \
  "the conversation is a lazy list" "$U" \
  's.replace("LazyColumn(", "Column(")'

scenario "the transcript is still inside the scrolling column" \
  "the log view is added directly to that scroll container" "$U" \
  's.replace("text = state.log,", "text = \"\",")'

scenario "the conversation keeps a minimum height" \
  "the log has a guaranteed minimum height" "$U" \
  's.replace("            .heightIn(min = 120.dp),\n", "")'

scenario "the conversation is not nested in a second scroller" \
  "the log is not nested inside a second scroll view" "$U" \
  's.replace("    LazyColumn(", "    Column(Modifier.verticalScroll(rememberScrollState())).apply { LazyColumn(")'

# --- the pre-existing checks, so this script is a whole-gate control ------

scenario "model preparation is still its own function" \
  "model preparation exists as its own function" "$M" \
  's.replace("private fun prepareModel(): Boolean", "private fun prepareModelX(): Boolean")'

scenario "the asset copy is still guarded" \
  "the asset copy is inside a try/catch" "$M" \
  's.replace("catch (e: Throwable)", "catch (e: java.io.IOException)")'

scenario "log lines still reach logcat" \
  "every log line is mirrored to logcat" "$M" \
  's.replace("android.util.Log.i(LOG_TAG, line)", "Unit")'

printf '\nANDROID-AUTOSTART-NEGATIVE: %d proved, %d could not fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
