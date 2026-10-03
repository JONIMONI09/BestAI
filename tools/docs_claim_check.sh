#!/usr/bin/env bash
# Documentation claim lint.
#
# WHY: an external audit found the project's own docs making claims its code
# disproves (a "Zero-RAM" title while errors.md recorded "Zero-RAM was an
# unproven claim"; a size table implying v1 holds 1.5 B parameters when the
# format caps at 256 KiB). Prose decays silently - nothing fails when a doc
# drifts. This gate makes specific banned claims fail CI.
#
# The list is deliberately small and precise. Over-broad patterns ("performance",
# "fast") would fire on honest caveats and train people to ignore the gate.
#
# Each entry: <id>|<regex>|<where to look>|<what to say instead>
# A match inside a line that is explicitly discussing the phrase is allowed -
# see the ALLOW list, so the docs can name the thing they no longer claim.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAIL=0

# Documentation under audit.
FILES=$(ls "$ROOT"/README.md "$ROOT"/docs/*.md "$ROOT"/android/README.md \
            "$ROOT"/tools/gpu_bench/README.md 2>/dev/null)
# Skill files and rules.md are audited too: they are where a hard-coded test
# count survives longest.
FILES="$FILES $(ls "$ROOT"/rules.md "$ROOT"/.claude/skills/*/SKILL.md 2>/dev/null)"

check() { # id  regex  human-explanation
  local id="$1" re="$2" why="$3" hits
  hits=$(grep -nEi "$re" $FILES 2>/dev/null \
         | grep -vE 'docs_claim_check|banned claim|no longer claims|was an unproven|previously claimed|ended up claiming' \
         || true)
  if [ -n "$hits" ]; then
    echo "FAIL  $id — $why" >&2
    printf '%s\n' "$hits" | sed 's/^/        /' >&2
    FAIL=1
  else
    echo "ok    $id"
  fi
}

# 1. Zero-RAM is physically impossible: mapped file pages and page cache are
#    real physical memory. errors.md has an entry saying exactly that.
check "no-zero-ram" \
  '(^|[^a-z-])(zero-ram|zero ram)([^a-z-]|$)' \
  'never claim zero RAM; say "no heap allocation for weights; residency is governed by the kernel page cache"'

# 2. "all models" - the reader supports F32/F16/BF16 GGUF tensors and a fixed
#    set of architectures. Quantised block formats are refused by name.
check "no-all-models" \
  '\ball (models|architectures|gguf|formats)\b' \
  'name the supported architectures and tensor types explicitly instead of "all"'

# 3. Hard-coded unit-test counts drift the moment a test is added and were the
#    single most-repeated stale number in this repo (rules.md said "49" for
#    years; the real x86 count is parsed from the binary, not assumed).
check "no-hardcoded-test-count" \
  '\b[0-9]{2,3} (unit )?tests\b|\b[0-9]{2,3} Tests,' \
  'parse the count from the test binary output; see tools/test_count_baseline.txt'

# 4. Performance figures with no provenance. R32 requires naming the machine.
#    Per-FILE, not per-line: quoting a benchmark sample is fine, but a document
#    that quotes numbers and never says where they came from is the failure.
PERF_RE='"?(ns_per_token|ns_per_step|rss_delta_kb|load_ms)"?:[[:space:]]*[0-9]'
PROV_RE='[Mm]easurement provenance|[Mm]easured on|not measured|SYNTHETIC|container|NOT MEASURED|host x86-64'
perf_bad=0
for f in $FILES; do
  [ -f "$f" ] || continue
  if grep -Eq "$PERF_RE" "$f" 2>/dev/null && ! grep -Eq "$PROV_RE" "$f" 2>/dev/null; then
    echo "FAIL  no-unqualified-perf — ${f#$ROOT/} quotes benchmark numbers but never names the host" >&2
    perf_bad=1
  fi
done
if [ "$perf_bad" -eq 0 ]; then
  echo "ok    no-unqualified-perf"
else
  FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
  printf '\nDOC-CLAIM-CHECK: all banned claims absent\n'
else
  printf '\nDOC-CLAIM-CHECK: FAILED — see above\n' >&2
fi
exit "$FAIL"
