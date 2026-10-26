#!/usr/bin/env bash
# Local Kotlin type-check of the Android app sources WITHOUT the Android SDK.
#
# What this catches: unresolved references, wrong signatures, missing imports,
# type errors - i.e. exactly the class of failures that made main red
# (HydraApp.kt:1011 'ClosedFloatingPointRange' has no .last/.first).
# What this is NOT: the real AGP build (resources, manifest, compose compiler
# plugin, lint checks). Run the CI for those.
#
# Setup (once, npm registry only - no apt):
#   mkdir -p /tmp/ktnpm && cd /tmp/ktnpm
#   npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8
# Run:
#   bash tools/kotlin_stub_check/kotlin_stub_check.sh
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
KOTLINC="${KOTLINC:-/tmp/ktnpm/node_modules/kotlin-compiler/bin/kotlinc}"
JRE="${JRE:-/tmp/ktnpm/node_modules/javajre-linux-64/jre/bin}"
if [[ ! -x "$KOTLINC" ]]; then
    echo "kotlinc not found. Setup: mkdir -p /tmp/ktnpm && cd /tmp/ktnpm && npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8"
    exit 2
fi
export PATH="$JRE:$PATH"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
(cd "$REPO_ROOT/tools/kotlin_stub_check/stubs" && "$KOTLINC" *.kt -d "$WORK/stubs.jar" 2>"$WORK/stubs.log")
if [[ -s "$WORK/stubs.log" ]] && grep -q "error:" "$WORK/stubs.log"; then
    cat "$WORK/stubs.log"; exit 2
fi
"$KOTLINC" \
    "$REPO_ROOT"/android/app/src/main/java/dev/hydrastone/*.kt \
    "$REPO_ROOT"/android/app/src/main/java/dev/hydrastone/ui/*.kt \
    "$REPO_ROOT"/android/app/src/main/java/dev/hydrastone/ui/theme/*.kt \
    -cp "$WORK/stubs.jar" -d "$WORK/app" 2>&1 | tee "$WORK/app.log"
if grep -q "error:" "$WORK/app.log"; then
    echo "kotlin stub check: FAILED"
    exit 1
fi
echo "kotlin stub check: OK (0 errors)"
