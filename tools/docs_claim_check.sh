#!/usr/bin/env bash
# Documentation claim lint.
#
# WHY: an external audit found the project's own docs making claims its code
# disproves (a "Zero-RAM" title while .agents/errors.md recorded "Zero-RAM was an
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

# Documentation under audit. An ARRAY, not a space-joined string: a path
# containing a space was split into words, grep failed on the non-existent
# half, and 2>/dev/null swallowed the error - so the file was skipped and the
# lint still reported success.
FILES=("$ROOT"/README.md "$ROOT"/docs/*.md \
       "$ROOT"/android/README.md \
       "$ROOT"/tools/gpu_bench/README.md)
# .agents/docs/* is audited too — except SESSION-ARCHIVE.md. The archive is
# a VERBATIM historical record (the round logs moved out of session.md on
# 2026-10-26): it QUOTES past claims — the old audit table names "Zero-RAM"
# as the finding it caught, and old rounds quote the test counts of their
# time. This gate polices what the project CURRENTLY claims, not what a
# dated record says a previous round claimed or caught (same reason
# CHANGELOG.md is not audited). "Fixing" the archive to satisfy the gate
# would rewrite history; the archive is append-only.
for f in "$ROOT"/.agents/docs/*.md; do
  case "$f" in *SESSION-ARCHIVE.md) ;; *) FILES+=("$f") ;; esac
done
# Skill files and .agents/rules.md are audited too: they are where a hard-coded test
# count survives longest. The skills are scanned at their CANONICAL location
# (.agents/skills/): on a checkout where the .claude/skills symlink is
# materialised as a text file, only the canonical glob still expands.
FILES+=("$ROOT"/.agents/rules.md "$ROOT"/.agents/skills/*/SKILL.md)

check() { # id  regex  human-explanation
  local id="$1" re="$2" why="$3" hits
  # The allow-list is matched against the CONTENT only, never the path.
  # grep -n prefixes every hit with "file:line:", so an unanchored pattern
  # meant that renaming a file to docs_claim_check_notes.md exempted it from
  # every check - a banned claim could be hidden by choosing a filename.
  hits=$(grep -nEi "$re" "${FILES[@]}" 2>/dev/null \
         | grep -vE '^[^:]*:[0-9]+:.*(docs_claim_check|banned claim|no longer claims|was an unproven|previously claimed|ended up claiming)' \
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
#    real physical memory. .agents/errors.md has an entry saying exactly that.
check "no-zero-ram" \
  '(^|[^a-z-])(zero-ram|zero ram)([^a-z-]|$)' \
  'never claim zero RAM; say "no heap allocation for weights; residency is governed by the kernel page cache"'

# 2. "all models" - the reader supports F32/F16/BF16 GGUF tensors and a fixed
#    set of architectures. Quantised block formats are refused by name.
check "no-all-models" \
  '\ball (models|architectures|gguf|formats)\b' \
  'name the supported architectures and tensor types explicitly instead of "all"'

# 3. Hard-coded unit-test counts drift the moment a test is added and were the
#    single most-repeated stale number in this repo (.agents/rules.md said "49" for
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
for f in "${FILES[@]}"; do
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

# Negative control (R25): prove the detectors can fire. A file carrying
# each banned pattern must be detected by its regex, and a clean line must
# not match any of them — a detector that cannot tell the two apart fails
# the gate (a lint that cannot fail is worthless; see README "Quality Gates").
NC_RE1='(^|[^a-z-])(zero-ram|zero ram)([^a-z-]|$)'
NC_RE2='\ball (models|architectures|gguf|formats)\b'
NC_RE3='\b[0-9]{2,3} (unit )?tests\b|\b[0-9]{2,3} Tests,'
nc=$(mktemp)
printf '%s\n' 'The engine is Zero-RAM, supports all models, and has 49 unit tests.' > "$nc"
for re in "$NC_RE1" "$NC_RE2" "$NC_RE3"; do
  if ! grep -qEi "$re" "$nc"; then
    echo "NEGATIVE CONTROL: FAIL — banned pattern not detected: $re" >&2
    rm -f "$nc"; exit 1
  fi
done
printf '%s\n' 'No banned patterns on this line.' > "$nc"
if grep -qEi "$NC_RE1|$NC_RE2|$NC_RE3" "$nc"; then
  echo "NEGATIVE CONTROL: FAIL — clean line matched a banned pattern" >&2
  rm -f "$nc"; exit 1
fi
rm -f "$nc"
echo "ok    negative control (banned patterns detected, clean line accepted)"

if [ "$FAIL" -eq 0 ]; then
  printf '\nDOC-CLAIM-CHECK: all banned claims absent\n'
else
  printf '\nDOC-CLAIM-CHECK: FAILED — see above\n' >&2
fi
exit "$FAIL"
