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
# compare the BEHAVIOURAL lines only: [Timing] is a measurement,
# [Hydra] is a summary header (the text side cannot know its prompt
# tokens before dispatch, so its Eingangs-Token differs), [Text] is
# the input echo that only exists on the text side.
norm_behaviour() { grep -vE '^\[(Timing|Text)\]:|^\[Hydra\] '; }
A="$(./hydra-run "$TMP/vocab.hydra2" 0 8 --text-prompt abcd)"
B="$(./hydra-run "$TMP/vocab.hydra2" 0 8 --prompt 5,6)"
[ "$(echo "$A" | norm_behaviour)" = "$(echo "$B" | norm_behaviour)" ] \
  || fail "--text-prompt abcd and --prompt 5,6 diverge"
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

# 5) repeat-penalty ring (round 13, Qodo #16/#22):
#    a) window 0 must not crash (pre-fix: `recent_n++ % 0` -> SIGFPE).
R0="$(./hydra-run "$TMP/vocab.hydra2" 0 4 --rep-penalty 2 --rep-window 0)" \
  || fail "--rep-window 0 crashed (division by zero in the ring)"
echo "$R0" | grep -q '^\[Tokens\]:' || fail "--rep-window 0 produced no tokens"
#    b) wraparound pin: prompt 0,1,5,2,6 with window 3 must end with
#    the ring holding exactly {2,5,6} (the last three prompt tokens).
#    The pre-fix ring wrote EVERY post-fill token into slot 0, so the
#    ring was {6,1,5} (token 1 penalized instead of token 2) and this
#    stream was different. Greedy (default temp 0) keeps the pin
#    deterministic.
RW="$(./hydra-run "$TMP/vocab.hydra2" 0 8 --prompt 0,1,5,2,6 \
       --rep-penalty 2 --rep-window 3 | grep '^\[Tokens\]:')" \
  || fail "ring wraparound run failed"
# (pin 2026-10-30, round 13, WITH prompt prefill active: the pre-fix
# slot-0-frozen ring produced `0 6 0 6 0 6 0 6` on this exact run)
echo "$RW" | grep -q '^\[Tokens\]: 0 6 0 6 0 6 2 7 $' \
  || fail "ring wraparound stream is not the pinned one (last-3 = {2,5,6})"

# 6) positional defaults apply to .hydra2 files too (round 13,
#    Qodo #23: the dispatch used to precede the default/clamp block,
#    so a bare `hydra-run model.hydra2` ran with start=-1/steps=-1):
#    no positionals -> defaults (42 -> %16 = 10, 16 steps), and the
#    JSON must carry exactly 16 tokens.
J0="$(./hydra-run "$TMP/vocab.hydra2" --json)" \
  || fail "hydra-run on a .hydra2 without positional args failed"
python3 - "$J0" <<'PY' || fail "positional defaults (42/16) broken for .hydra2"
import json, sys
d = json.loads(sys.argv[1])
assert len(d["tokens"]) == 16, d
assert d["start_token"] == 10, d   # 42 % 16 (vocab 16)
PY

echo "text_prompt_check: ok (BPE parity, JSON echo, loud refusals, ring + defaults)"
