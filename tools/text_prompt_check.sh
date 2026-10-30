#!/usr/bin/env bash
# Gate: the --text-prompt CLI (M4: embedded vocab + BPE, round 12).
#
# WHY
# ---
# --text-prompt encodes a user text into token ids with the model's
# embedded BPE vocabulary and feeds them as the prompt. The encoding
# is the whole feature: a wrong BPE application, a missing loud
# refusal on a vocab-less file, or a malformed JSON echo would all
# pass the unit tests (which drive the codec directly) while the CLI
# still lies to the user. This gate drives the real binary.
set -euo pipefail
cd "$(dirname "$0")/.."

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 tools/make_model.py "$TMP/vocab.hydra2" --moe-full --layers 2 >/dev/null
python3 tools/make_model.py "$TMP/novocab.hydra2" --moe-full --layers 2 --moe-no-vocab >/dev/null
python3 tools/make_model.py "$TMP/v1.hydra" >/dev/null

fail() { echo "text_prompt_check: $1" >&2; exit 1; }

# 1) 'abcd' encodes to [5,6] (merges (0,1)->5, (2,3)->6): the stream
#    with the text prompt must equal the stream with the numeric
#    prompt - byte for byte.
# (the [Timing] line is a measurement, the [Text] echo exists only on
# the text side - both are excluded from the byte comparison)
A="$(./hydra-run "$TMP/vocab.hydra2" 0 8 --text-prompt abcd | grep -v '^\[Timing\]')"
B="$(./hydra-run "$TMP/vocab.hydra2" 0 8 --prompt 5,6 | grep -v '^\[Timing\]')"
[ "$(echo "$A" | grep -v '^\[Text\]:')" = "$B" ] || fail "--text-prompt abcd and --prompt 5,6 diverge"
echo "$A" | grep -q '^\[Text\]: abcd$' || fail "missing [Text]: echo line"
echo "$A" | grep -q '^\[Prompt\]: 5 6 $' || fail "encoded prompt is not 5 6"

# 2) BPE semantics end to end: 'abc' -> [5,2], 'hello' is unmappable.
C="$(./hydra-run "$TMP/vocab.hydra2" 0 8 --text-prompt abc)"
echo "$C" | grep -q '^\[Prompt\]: 5 2 $' || fail "'abc' did not encode to [5,2]"
if ./hydra-run "$TMP/vocab.hydra2" 0 4 --text-prompt hello >/dev/null 2>&1; then
  fail "'hello' (unmapped byte) was not refused"
fi

# 3) JSON echo: the text field must be present, escaped and parseable.
J="$(./hydra-run "$TMP/vocab.hydra2" 0 3 --text-prompt abcd --json)"
python3 - "$J" <<'PY' || fail "JSON text field broken"
import json, sys
d = json.loads(sys.argv[1])
assert d["text"] == "abcd", d
assert d["prompt"] == [5, 6], d
PY

# 4) loud refusals: no embedded vocab, v1 file, flag combination.
if ./hydra-run "$TMP/novocab.hydra2" 0 4 --text-prompt abcd >/dev/null 2>&1; then
  fail "text prompt on a vocab-less .hydra2 was not refused"
fi
if ./hydra-run "$TMP/v1.hydra" 0 4 --text-prompt abcd >/dev/null 2>&1; then
  fail "text prompt on a v1 .hydra file was not refused"
fi
if ./hydra-run "$TMP/vocab.hydra2" 0 4 --text-prompt ab --prompt 5 >/dev/null 2>&1; then
  fail "--text-prompt and --prompt together were not refused"
fi

echo "text_prompt_check: ok (BPE parity, JSON echo, loud refusals)"
