#!/usr/bin/env bash
# Negative control for tools/string_format_check.py.
#
# WHY THIS EXISTS
# ---------------
# The gate was written in the same change that fixed the crash it exists to
# catch. A brand new gate has never been seen to fail, so under rules.md R25 it
# is not yet a gate - it is a hope. This script is the proof, and it lives in
# the repository and runs in CI so the proof cannot rot.
#
# Each scenario removes exactly one property the gate relies on and requires
# the gate to go red. A scenario that failed for some unrelated reason would
# make the control look stronger than it is, so every scenario states what it
# broke and the control checks that the gate's failure MESSAGE names that same
# property - a gate that fails for the wrong reason has still not proved it
# can fail for the right one.
#
# It never touches the repository. Every scenario builds a throwaway tree under
# a temporary directory with its own copy of the gate, its own strings.xml and
# its own Kotlin sources. The gate derives its root from its own location, so a
# copied tree is a self-contained world.
#
# Usage: bash tools/string_format_negative_test.sh
# Exit 0 means every check was proved capable of failing.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GATE="$ROOT/tools/string_format_check.py"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }

[ -f "$GATE" ] || { echo "missing $GATE" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

RESOURCES="android/app/src/main/res/values/strings.xml"
KT="android/app/src/main/java/dev/hydrastone"

# A minimal but complete world: the gate reads exactly these three things.
seed_fixture() {
    local dir="$1"
    rm -rf "$dir"
    mkdir -p "$dir/tools"
    mkdir -p "$dir/android/app/src/main/res/values"
    mkdir -p "$dir/android/app/src/main/java/dev/hydrastone/ui"
    cp "$GATE" "$dir/tools/string_format_check.py"
    cat > "$dir/android/app/src/main/res/values/strings.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">Fixture</string>
    <string name="one_arg">value %1$s</string>
    <string name="two_args">value %1$s then %2$s</string>
    <string name="no_args">plain</string>
    <plurals name="one_plural">
        <item quantity="one">%1$d iteration</item>
        <item quantity="other">%1$d iterations</item>
    </plurals>
</resources>
XML
    cat > "$dir/android/app/src/main/java/dev/hydrastone/MainActivity.kt" <<'KT'
package dev.hydrastone
class MainActivity {
    fun a(ctx: Any) {
        val x1 = ctx.getString(R.string.one_arg, "a")
        val x2 = ctx.getString(
            R.string.two_args,
            "a",
            "b",
        )
        val x3 = ctx.getString(R.string.no_args)
        val x4 = ctx.resources.getQuantityString(R.plurals.one_plural, 3, 3)
    }
}
KT
}

# Runs the gate in the fixture and prints its exit status.
run_gate() {
    local dir="$1"
    python3 "$dir/tools/string_format_check.py" 2>&1
}

expect_pass() {
    local name="$1"
    local dir="$2"
    local out rc
    out="$(run_gate "$dir")"; rc=$?
    if [ "$rc" -eq 0 ]; then
        ok "$name"
    else
        bad "$name" "the gate went red on a fixture it should accept: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    fi
}

expect_fail_naming() {
    local name="$1" dir="$2" needle="$3"
    local out rc
    out="$(run_gate "$dir")"; rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "$name" "the gate stayed GREEN on a broken fixture"
        return 0
    fi
    if printf '%s' "$out" | grep -qi -- "$needle"; then
        ok "$name"
    else
        bad "$name" "the gate went red but its output never mentioned '$needle': $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    fi
}

# ---------------------------------------------------------------------------
# 0. Control: the unmodified fixture passes. Without this, "the gate fails"
#    would prove nothing - a gate that always fails passes every scenario.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/ctl"
expect_pass "the unmodified control fixture passes" "$WORK/ctl"

# ---------------------------------------------------------------------------
# 1. The exact crash this gate was written for: a plurals entry reaches for a
#    second format argument that the quantity does not supply.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/plurals_index"
sed -i 's|<item quantity="other">%1\$d iterations</item>|<item quantity="other">%2$d iterations</item>|' \
    "$WORK/plurals_index/android/app/src/main/res/values/strings.xml"
expect_fail_naming "a plurals entry reaching for a second argument is caught" \
    "$WORK/plurals_index" "plurals/one_plural"

# ---------------------------------------------------------------------------
# 2. A plurals entry read with no format argument at all. This is a different
#    check from 1: 1 is "the index is too high", 2 is "there is no argument".
# ---------------------------------------------------------------------------
seed_fixture "$WORK/plurals_none"
sed -i 's|getQuantityString(R.plurals.one_plural, 3, 3)|getQuantityString(R.plurals.one_plural, 3)|' \
    "$WORK/plurals_none/android/app/src/main/java/dev/hydrastone/MainActivity.kt"
expect_fail_naming "a plurals entry read with no format argument is caught" \
    "$WORK/plurals_none" "plurals/one_plural"

# ---------------------------------------------------------------------------
# 3. A plain string that needs two arguments and is given one.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/short_args"
python3 - "$WORK/short_args/android/app/src/main/java/dev/hydrastone/MainActivity.kt" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('R.string.two_args,\n            "a",\n            "b",\n', 'R.string.two_args,\n            "a",\n')
open(p, "w").write(s)
PY
expect_fail_naming "a string short of a format argument is caught" \
    "$WORK/short_args" "string/two_args"

# ---------------------------------------------------------------------------
# 4. The same fixture with its argument restored MUST go green again. Without
#    this, scenario 3 would also pass for a gate that fails on any two-argument
#    string at all.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/fixed_args"
expect_pass "the two-argument string passes again once the argument is restored" \
    "$WORK/fixed_args"

# ---------------------------------------------------------------------------
# 5. The gate must notice it is not looking at the resources at all.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/no_strings"
rm -f "$WORK/no_strings/android/app/src/main/res/values/strings.xml"
expect_fail_naming "a missing strings.xml is caught" \
    "$WORK/no_strings" "strings.xml exists"

# ---------------------------------------------------------------------------
# 6. A resources file with no format specifiers anywhere: the gate must not
#    report a clean sweep it never actually checked.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/no_specs"
python3 - "$WORK/no_specs/android/app/src/main/res/values/strings.xml" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r"%[0-9]*\$[sd]", "X", s)
open(p, "w").write(s)
PY
expect_fail_naming "a resources file with no specifiers is caught, not waved through" \
    "$WORK/no_specs" "no <string>/<plurals> entry with a specifier was parsed"

# ---------------------------------------------------------------------------
# 7. A bare "%d" is index 1, not "no index": an entry with two of them and one
#    argument must be caught. If the gate read a bare specifier as "no
#    specifier at all" it would wave this through.
# ---------------------------------------------------------------------------
seed_fixture "$WORK/bare_spec"
python3 - "$WORK/bare_spec/android/app/src/main/res/values/strings.xml" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
# Two bare specifiers, so "bare %d counts as index 1" is what makes this fail:
# if the gate read a bare "%d" as "no specifier at all" it would go green.
s = s.replace('<string name="one_arg">value %1$s</string>',
              '<string name="one_arg">value %d and %d</string>')
open(p, "w").write(s)
PY
expect_fail_naming "a bare %d counts as a real specifier, not as none" \
    "$WORK/bare_spec" "string/one_arg"

printf '\nSTRING-FORMAT-NEGATIVE: %d proved, %d could not fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
printf 'STRING-FORMAT-NEGATIVE: ok\n'
exit 0