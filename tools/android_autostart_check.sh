#!/usr/bin/env bash
# Gate: the Android app runs on its own, and a missing model never kills it.
#
# Symptom this pins down: "on Android it simply does not run through". Two
# separate causes, both invisible to the host test suite:
#
#   1. Nothing started a run until the user pressed Run inference, so the
#      app opened to an empty log and looked broken.
#   2. The asset copy sat unguarded inside onCreate(). A missing or
#      unreadable asset threw on the main thread, the activity died, and
#      the user saw no window at all - which is exactly what "it does
#      nothing" looks like from the outside.
#
# Each check below is a grep with a stated reason. The negative control is
# the point: delete prepareModel()'s guard and this gate has to fail, or it
# is decoration.
#
# The UI is Compose, so a check has to know which file a behaviour lives in.
# MainActivity.kt orchestrates; ui/HydraApp.kt renders. Both are checked,
# and every check below names the file it asserts against, because "the
# check moved from one file to another" and "the behaviour is gone" look
# identical from the outside if the gate only ever greps one of them.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KT="$ROOT/android/app/src/main/java/dev/hydrastone/MainActivity.kt"
UI="$ROOT/android/app/src/main/java/dev/hydrastone/ui/HydraApp.kt"
STRINGS="$ROOT/android/app/src/main/res/values/strings.xml"
ASSET="$ROOT/android/app/src/main/assets/starter.hydra"
# Same file, relative to the repo root - git check-ignore wants that form.
REL_ASSET="android/app/src/main/assets/starter.hydra"
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; }

need() { # need <label> <file> <extended-regex>
  if grep -Eq -- "$3" "$2" 2>/dev/null; then ok "$1"; else bad "$1" "pattern not found: $3"; fi
}

notneed() { # notneed <label> <file> <extended-regex>
  if grep -Eq -- "$3" "$2" 2>/dev/null; then bad "$1" "pattern unexpectedly present: $3"; else ok "$1"; fi
}

printf '== Android auto-start and model preparation ==\n'

[ -f "$KT" ] || { bad "MainActivity.kt exists" "missing: $KT"; printf '\nANDROID-AUTOSTART-CHECK: %d passed, %d failed\n' "$PASS" "$FAIL"; exit 1; }
[ -f "$UI" ] || { bad "HydraApp.kt exists" "missing: $UI"; printf '\nANDROID-AUTOSTART-CHECK: %d passed, %d failed\n' "$PASS" "$FAIL"; exit 1; }
[ -f "$STRINGS" ] || { bad "strings.xml exists" "missing: $STRINGS"; }

need "model preparation exists as its own function" "$KT" 'fun prepareModel\(\): *Boolean'
need "prepareModel is called from onCreate" "$KT" 'if *\(!prepareModel\(\)\)'
need "a failed preparation returns early instead of continuing" "$KT" 'if *\(!prepareModel\(\)\) *\{'
need "the asset copy is inside a try/catch" "$KT" 'catch *\(e: *Throwable\)'
need "the starter asset is read by name, not assumed" "$KT" 'assets\.open\(STARTER_ASSET\)'
need "the asset name is a constant, not a literal in two places" "$KT" 'const val STARTER_ASSET *= *"starter\.hydra"'

# The specific defect: assets.open() unwrapped inside onCreate(). If the
# guard is removed, the app dies before it draws anything.
if awk '/override fun onCreate/,/^    private fun [a-z]/' "$KT" \
     | grep -E 'assets\.open' >/dev/null 2>&1; then
  bad "no unguarded assets.open() inside onCreate" \
      "prepareModel() must own the copy; onCreate must not call assets.open itself"
else
  ok "no unguarded assets.open() inside onCreate"
fi

need "a run is started automatically after preparation" "$KT" 'log_auto_run'

# Compose replaced root.post { startInference() } with a LaunchedEffect, so
# the invariant is now "the auto-run effect actually calls startInference()".
# Two independent greps would both pass if the effect existed but did nothing
# while a bare startInference() call sat elsewhere in the file, so this is
# checked as an ORDER inside one window, the same way android_crash_check.sh
# checks that a report is written before the crash is delegated.
if awk '/LaunchedEffect\(Unit\)/,/^                \}/' "$KT" \
     | grep -Eq 'startInference\(\)'; then
  ok "the automatic run goes through the same path as the button"
else
  bad "the automatic run goes through the same path as the button" \
      "the LaunchedEffect that replaced root.post { startInference() } does not call startInference()"
fi

need "the auto-run switch exists" "$UI" 'Switch\('
need "the switch is labelled from resources, not hardcoded" "$UI" 'stringResource\(R\.string\.action_auto_run\)'
need "the switch state is readable" "$KT" 'autoRun *= *checked'

need "an imported model wins over the starter model" "$KT" 'IMPORTED_NAME'
need "the active model is shown in the UI" "$UI" 'text = ".*modelLabel'
need "the model label text comes from resources" "$UI" 'stringResource\(R\.string\.label_model\)'

# A send button that is enabled with no model loaded does nothing when pressed.
# Scoped to the composer on purpose: `enabled = state.hasModel` also appears on
# the benchmark button, so an unscoped grep would still pass after the SEND
# button's guard was deleted - which is the regression this check exists for.
if awk '/private fun InputRow/,/^}/' "$UI" | grep -Eq 'enabled *= *state\.hasModel'; then
  ok "no model means the run button is disabled"
else
  bad "no model means the run button is disabled" \
      "the composer's send button is not gated on hasModel"
fi
need "no model means an explanation is shown" "$KT" 'R\.string\.err_no_model'
need "an import also starts a run straight away" "$KT" 'startInference\(\)'

# The log must be readable without the UI. Reading the TextView back on a
# headless emulator needs uiautomator, which under TCG is heavy enough to
# starve and kill the guest - so the same lines go to logcat.
need "every log line is mirrored to logcat" "$KT" 'Log\.i\(LOG_TAG, line\)'
need "the logcat tag is a constant, not an inline literal" "$KT" 'const val LOG_TAG *= *"Hydra"'

# The output has to be REACHABLE. A vertical LinearLayout that is taller than
# the screen lays its last children out below the bottom edge, and on a
# 320x640 device that put the whole token log out of sight: the engine ran
# and the user saw nothing.
#
# In Compose the same defect has two shapes: a column that does not scroll at
# all, and a transcript rendered outside the container that does.
need "the whole screen is scrollable" "$UI" 'verticalScroll\(rememberScrollState\(\)\)'
need "the conversation is a lazy list" "$UI" 'LazyColumn\('

# The transcript must sit INSIDE the scrolling container. Grepping for both
# patterns independently would pass if the transcript were rendered outside
# the scrollable column, which is exactly the original defect, so the order
# inside one window is what is asserted.
if awk '/verticalScroll\(rememberScrollState\(\)\)/,/^private fun SettingsTab/' "$UI" \
     | grep -q 'text = state\.log'; then
  ok "the log view is added directly to that scroll container"
else
  bad "the log view is added directly to that scroll container" \
      "state.log is not rendered inside the scrolling column"
fi

# The fixed heightIn(min = 120.dp) floor is gone on purpose: it overflowed
# the window exactly when the window was smallest (keyboard open on a short
# screen) and pushed the composer off screen. The floor is now relative -
# AdaptiveHeaderSplit caps the header block at headerFraction of the space
# and measures the conversation with the remainder, so the conversation
# always keeps (1 - headerFraction) of the space above the composer, and
# the composer itself is a plain sibling after the weighted split.
#
# Also scoped: the patterns below would match any comment anywhere in the
# file, so the awk window is what asserts they live inside the split's
# measurement logic. And the first one greps the CAP EXPRESSION
# (maxHeight * headerFraction), not the parameter alone: a parameter that
# nothing multiplies is a cap in name only.
if awk '/private fun AdaptiveHeaderSplit/,/^}/' "$UI" \
     | grep -Eq 'maxHeight \* headerFraction'; then
  ok "the header block is capped at a fraction of the space"
else
  bad "the header block is capped at a fraction of the space" \
      "AdaptiveHeaderSplit no longer computes a cap from headerFraction"
fi
if awk '/private fun AdaptiveHeaderSplit/,/^}/' "$UI" \
     | grep -Eq 'maxHeight - headerPlaceable\.height'; then
  ok "the conversation gets the space left over from the header"
else
  bad "the conversation gets the space left over from the header" \
      "the split no longer hands the remainder to the content"
fi

# Two same-direction scroll containers fight each other and the outer one
# cannot reach the inner content. In Views that was ScrollView-in-ScrollView;
# in Compose it is a LazyColumn placed inside a verticalScroll column. The
# adaptive header IS scrollable - that is the point - but the transcript
# must not be inside it.
if awk '/private fun MessageList/,/^}/' "$UI" \
     | grep -Eq 'verticalScroll\('; then
  bad "the log is not nested inside a second scroll view" \
      "the conversation LazyColumn is nested inside a verticalScroll column"
else
  ok "the log is not nested inside a second scroll view"
fi

notneed "the demo model name is gone from the app" "$KT" 'demo\.hydra'
notneed "no demo.hydra reference anywhere in the Android sources" \
  "$ROOT/android/app/src/main" 'demo\.hydra'

if [ -f "$ASSET" ]; then
  ok "the starter model is shipped in the assets ($ASSET)"
  if [ "$(wc -c < "$ASSET")" -gt 24 ]; then
    ok "the starter model is larger than its 24-byte header"
  else
    bad "the starter model is larger than its 24-byte header"
  fi
else
  bad "the starter model is shipped in the assets" "missing: $ASSET"
fi

# The byte-comparison below runs against the file on disk, so it PASSES on a
# developer machine that happens to have the asset and fails only in CI, where
# a fresh checkout does not. That is exactly how demo.hydra -> starter.hydra
# shipped an APK with no model in it: .gitignore ignores *.hydra, the renamed
# file was never staged, and nothing failed until GitHub Actions ran.
#
# So check the thing that actually broke: git must be willing to track it.
if command -v git >/dev/null 2>&1 && [ -d "$ROOT/.git" ]; then
  # --no-index is load-bearing: by default git check-ignore skips paths that
  # are already tracked, so the moment someone fixes the .gitignore and stages
  # the file, this check would report "ok" forever and stop catching the
  # regression. --no-index asks the pure question we care about: if this file
  # were added fresh, would .gitignore refuse it?
  if (cd "$ROOT" && git check-ignore -q --no-index "$REL_ASSET"); then
    bad "the starter model is NOT git-ignored" \
        "$REL_ASSET is ignored by .gitignore, so it is missing from every fresh checkout"
  else
    ok "the starter model is NOT git-ignored"
  fi
fi

# The shipped asset must be a real model, not the header alone, and it must
# be byte-identical to what tools/make_model.py produces - otherwise the APK
# ships a stale blob that does not match the generator in the repo.
if command -v python3 >/dev/null 2>&1; then
  if python3 - "$ASSET" "$ROOT/tools/make_model.py" <<'PY'
import importlib.util, sys
asset, gen = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location('make_model', gen)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
sys.exit(0 if open(asset, 'rb').read() == mod.build() else 1)
PY
  then ok "the shipped starter model matches tools/make_model.py byte for byte"
  else bad "the shipped starter model matches tools/make_model.py byte for byte"; fi
fi

printf '\nANDROID-AUTOSTART-CHECK: %d checks passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]