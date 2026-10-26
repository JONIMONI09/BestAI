#!/usr/bin/env bash
# Gate 1i: actionlint over every workflow file.
#
# Regression for the 2026-10-26 "phantom" zero-job runs: a workflow file
# that names an event GitHub's Actions compiler does not accept as a
# workflow trigger (e.g. `code_scanning_alert`, which is a webhook event
# only) is REJECTED at load time - the run is created with zero jobs,
# fails in the same second, and its name is the file PATH. PyYAML and
# the duplicate-key gate (1h) validate YAML syntax, not the Actions
# event/action schema; actionlint validates both. See .agents/errors.md
# (round 6, "code_scanning_alert is NOT a workflow trigger event").
#
# The npm package `actionlint` is a WASM build of the Go linter: no apt,
# no Go toolchain, runs on the plain Node we already have. /tmp is
# ephemeral between turns - the script recreates its setup if gone
# (recreate command also recorded in .agents/Environment.md).
#
# FAIL-CLOSED RULES (both learned from live behavior, 2026-10-26):
#   * every file is linted in its OWN node process (a fresh WASM
#     runtime) - the build crashes intermittently (wasm RuntimeError:
#     unreachable) when it lints several files in one process;
#   * a crash is never read as "clean": crashed attempts are retried up
#     to 3 times, and a file that still crashes fails the gate;
#   * the negative control must end in FINDINGS (exit 1) within 3
#     attempts: exit 0 means the detector is blind, persistent exit 2
#     means the linter is dead - both fail the gate.
set -euo pipefail
cd "$(dirname "$0")/.."

AL_DIR=/tmp/al
if [ ! -f "$AL_DIR/node_modules/actionlint/node.cjs" ] || [ ! -f "$AL_DIR/lint_one.js" ]; then
  mkdir -p "$AL_DIR"
  (cd "$AL_DIR" && npm install --no-fund --no-audit --loglevel=error actionlint)
  cat > "$AL_DIR/lint_one.js" <<'EOF'
// One workflow file per process (see the fail-closed rule in the gate).
// exit 0 = clean, exit 1 = findings, exit 2 = linter crashed (setup or
// lint step threw).
const fs = require('fs');
(async () => {
  let linter;
  try {
    const mod = require('actionlint'); // node+require -> node.cjs via package exports
    linter = await mod.createLinter();
  } catch (e) {
    console.error('actionlint setup failed: ' + e);
    process.exit(2);
  }
  const f = process.argv[2];
  let res;
  try {
    res = linter(fs.readFileSync(f, 'utf8'), f);
  } catch (e) {
    console.error('actionlint crashed on ' + f + ': ' + e);
    process.exit(2);
  }
  if (res.length) {
    for (const r of res) console.log(`${f}:${r.line}:${r.column} [${r.kind}] ${r.message}`);
    process.exit(1);
  }
  console.log('ok   ' + f);
})();
EOF
fi

# lint_file <file> -> exit 0 clean, 1 findings, 2 crashed after 3 tries.
lint_file() {
  local f="$1" rc attempt
  for attempt in 1 2 3; do
    set +e
    node "$AL_DIR/lint_one.js" "$f"
    rc=$?
    set -e
    if [ "$rc" -eq 2 ]; then
      echo "warn: actionlint crashed on $f (attempt $attempt/3) - retrying" >&2
      continue
    fi
    return "$rc"
  done
  echo "FAIL: actionlint crashed on $f in all 3 attempts (linter unusable)" >&2
  return 2
}

# --- Gate: every real workflow file must lint clean ---
shopt -s nullglob
files=(.github/workflows/*.yml .github/workflows/**/*.yml)
shopt -u nullglob
# de-duplicate (the glob can list a file twice on some shells)
mapfile -t files < <(printf '%s\n' "${files[@]}" | sort -u)
if [ ${#files[@]} -eq 0 ]; then
  echo "WORKFLOW-LINT-CHECK: FAIL - no workflow files found (path bug?)" >&2
  exit 1
fi
for f in "${files[@]}"; do
  rc=0
  lint_file "$f" || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "WORKFLOW-LINT-CHECK: FAIL - $f (findings above, or linter crash if rc=2)" >&2
    exit 1
  fi
done

# --- Negative control: an invalid trigger event MUST be flagged ---
# (R25: a gate that cannot fail is decoration. If actionlint ever stops
# catching unknown events, this fixture turns the gate red.)
rc=2
for attempt in 1 2 3; do
  set +e
  node "$AL_DIR/lint_one.js" tools/fixtures/workflow_lint_bad.yml > /dev/null 2>&1
  rc=$?
  set -e
  [ "$rc" -eq 2 ] && continue
  break
done
if [ "$rc" -eq 1 ]; then
  echo "negative control: invalid trigger event detected (as required)"
elif [ "$rc" -eq 0 ]; then
  echo "WORKFLOW-LINT-CHECK: FAIL - negative control NOT detected: actionlint accepted an invalid trigger event (gate is blind)" >&2
  exit 1
else
  echo "WORKFLOW-LINT-CHECK: FAIL - actionlint crashed on the negative control in all 3 attempts (linter unusable)" >&2
  exit 1
fi
echo "WORKFLOW-LINT-CHECK: ok (${#files[@]} workflow files linted, negative control proven)"
