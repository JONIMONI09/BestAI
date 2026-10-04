#!/usr/bin/env bash
# Gate: llama.cpp is pinned, and stays pinned.
#
# WHY
# ---
# The whole native integration is written against ONE llama.cpp API. Two
# things in that API moved between releases: the vocabulary became an opaque
# pointer returned by llama_model_get_vocab(), and end-of-generation moved to
# llama_vocab_is_eog(). A submodule that follows its default branch would break
# the build at some unrelated future commit, with an error that points at
# llama_jni.cpp rather than at the pin.
#
# So the pin is asserted, not assumed: the submodule must be recorded at
# exactly this commit. rules.md R15.
#
# Usage: bash tools/llama_pin_check.sh
# Exit 0 when the pin matches, 1 otherwise.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUBMODULE_PATH="android/app/src/main/cpp/llama"

PIN_TAG="v0.5.0"
PIN_COMMIT="7fe450e19305b828c199d602c23a8337aaa1f03b"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; [ $# -gt 1 ] && printf '     %s\n' "$2"; return 0; }

cd "$ROOT" || exit 1

# 1. the submodule is declared
if [ -f .gitmodules ] && grep -q "submodule \"llama\"" .gitmodules; then
  ok "llama.cpp is declared as a submodule"
else
  bad "llama.cpp is declared as a submodule" ".gitmodules has no llama entry"
fi

# 2. it is recorded in the INDEX at the pin. The index, not the working tree:
#    an uncommitted checkout would otherwise make this gate report whatever
#    happens to be on disk rather than what a fresh clone would get.
recorded="$(git ls-files -s "$SUBMODULE_PATH" | awk '{print $2}')"
if [ "$recorded" = "$PIN_COMMIT" ]; then
  ok "the recorded gitlink is $PIN_COMMIT ($PIN_TAG)"
else
  bad "the recorded gitlink is $PIN_COMMIT ($PIN_TAG)" \
      "index records '${recorded:-nothing}' - a fresh clone would get that commit instead"
fi

# 3. and the working tree agrees with the index, so a local checkout cannot
#    quietly build against different sources than CI does.
if git -C "$SUBMODULE_PATH" rev-parse HEAD >/dev/null 2>&1; then
  head="$(git -C "$SUBMODULE_PATH" rev-parse HEAD)"
  if [ "$head" = "$PIN_COMMIT" ]; then
    ok "the checked-out submodule is at $PIN_COMMIT"
  else
    bad "the checked-out submodule is at $PIN_COMMIT" "working tree is at $head"
  fi
else
  bad "the submodule is checked out" "$SUBMODULE_PATH is not a git checkout"
fi

# 4. no GGUF weights are tracked. tools/fetch_test_model.sh downloads them; a
#    committed one would add ~100 MB to the history and put third-party
#    weights under this repository's licence.
tracked_gguf="$(git ls-files | grep -ci '\.gguf$' || true)"
if [ "$tracked_gguf" -eq 0 ]; then
  ok "no GGUF weights are tracked"
else
  bad "no GGUF weights are tracked" "$tracked_gguf tracked .gguf file(s)"
fi

# 5. the licence of the vendored code is present, because a submodule is not
#    covered by this repository's own licence file.
if [ -f "$SUBMODULE_PATH/LICENSE" ] && grep -qi "MIT License" "$SUBMODULE_PATH/LICENSE"; then
  ok "the vendored llama.cpp carries its MIT licence"
else
  bad "the vendored llama.cpp carries its MIT licence" \
      "$SUBMODULE_PATH/LICENSE is missing or is not MIT"
fi

printf '\nLLAMA-PIN-CHECK: %d passed, %d failed - llama.cpp pinned at %s (%s), MIT\n' \
  "$PASS" "$FAIL" "$PIN_COMMIT" "$PIN_TAG"
[ "$FAIL" -eq 0 ] || exit 1
