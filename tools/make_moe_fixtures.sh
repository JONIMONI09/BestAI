#!/bin/sh
# Single source of truth for the hydra-moe-test fixture list.
#
# Round 16 follow-up (CI incident 41222651267): the list lived twice - in
# the Makefile's test-run target AND inline in ci.yml's sanitizer leg -
# and the two copies drifted when the Q4_0 pins (fixtures 8 and 9) were
# added to the Makefile only. The sanitizer leg then ran a 9-fixture
# binary with 7 fixtures and the main-tier CI failed on both matrix
# legs. There is now one generator: the Makefile and the CI sanitizer
# leg both call this script, so a new fixture is added in exactly one
# place.
#
# Usage: tools/make_moe_fixtures.sh <base>
#   Generates the nine .hydra2 fixtures under <base>{,-t64,-t128,
#   -t64-lg,-nv,-at64,-gqa,-q4,-q4f32} and PRINTS their paths, in the
#   exact order tests/test_hydra_moe.c expects them as argv[1..9]
#   (F32, ternary S64, ternary S128, ternary S64 large, no-vocab F32,
#   attention-ternary S64, GQA n_heads 4 / n_kv_heads 2, Q4_0,
#   Q4-same-shape F32).
set -eu

case "${1:-}" in
  -h|--help|"")
    echo "usage: tools/make_moe_fixtures.sh <base>" >&2
    exit 2
    ;;
esac
P="$1"

python3 tools/make_model.py "$P.hydra2"      --moe-full --layers 2
python3 tools/make_model.py "$P-t64.hydra2"  --moe-full --layers 2 --moe-ternary
python3 tools/make_model.py "$P-t128.hydra2" --moe-full --layers 2 --moe-ternary 128
python3 tools/make_model.py "$P-t64-lg.hydra2" --moe-full --layers 1 --experts 4 --top-k 2 --dim 256 --n-heads 8 --head-dim 32 --inter 64 --moe-ternary 64
python3 tools/make_model.py "$P-nv.hydra2"   --moe-full --layers 2 --moe-no-vocab
python3 tools/make_model.py "$P-at64.hydra2" --moe-full --layers 2 --moe-attn-ternary
python3 tools/make_model.py "$P-gqa.hydra2"  --moe-full --layers 2 --n-heads 4 --head-dim 4 --n-kv-heads 2
# round 16: the Q4_0 engine pins (layout 2) - the Q4_0 fixture and the
# same-shape F32 fixture it is cross-checked against
python3 tools/make_model.py "$P-q4.hydra2"   --moe-full --layers 2 --dim 64 --n-heads 2 --head-dim 32 --inter 64 --experts 4 --top-k 2 --moe-q4
python3 tools/make_model.py "$P-q4f32.hydra2" --moe-full --layers 2 --dim 64 --n-heads 2 --head-dim 32 --inter 64 --experts 4 --top-k 2

echo "$P.hydra2" "$P-t64.hydra2" "$P-t128.hydra2" "$P-t64-lg.hydra2" "$P-nv.hydra2" "$P-at64.hydra2" "$P-gqa.hydra2" "$P-q4.hydra2" "$P-q4f32.hydra2"
