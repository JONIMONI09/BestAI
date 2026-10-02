#!/usr/bin/env bash
# Replays the version-resolution logic of .github/workflows/release.yml
# locally, so the trigger behaviour is verified before the workflow runs on
# GitHub. The script body is read straight out of the workflow file, so this
# test cannot drift away from the YAML.
#
# Usage: bash tools/ci_release_version_test.sh [path/to/release.yml]
# Der optionale Pfad existiert fuer die Negativkontrolle: mit einer
# kaputten Kopie der Workflow-Datei muss der Test fehlschlagen (R25).
set -uo pipefail

WORKFLOW="${1:-.github/workflows/release.yml}"
fail=0

run_case() {
  local desc="$1" event="$2" ref="$3" tag="$4" publish="$5" expect_rc="$6"
  shift 6
  # Erwartungswerte werden nur im Erfolgsfall geprueft.
  local expect_tag="${1:-}" expect_version="${2:-}" expect_code="${3:-}" expect_pub="${4:-}"
  local ref_type="${REF_TYPE:-tag}" head_sha="${HEAD_SHA:-aaa}" tag_list="${TAG_LIST:-}"

  local out rc
  out="$(mktemp)"
  # Extract the run-script of the "Derive version from tag" step.
  local script
  script="$(python3 - "$WORKFLOW" <<'PY'
import sys, yaml
wf = yaml.safe_load(open(sys.argv[1]))
for step in wf["jobs"]["version"]["steps"]:
    if step.get("id") == "v":
        print(step["run"], end="")
        break
PY
)"

  EVENT="$event" REF_NAME="$ref" INPUT_TAG="$tag" INPUT_PUBLISH="$publish" \
  REF_TYPE="$ref_type" HEAD_SHA="$head_sha" TAG_LIST="$tag_list" \
  RUN_NUMBER="4242" GITHUB_OUTPUT="$out" GITHUB_STEP_SUMMARY="$out.summary" \
    bash -euo pipefail -c "$script" >/dev/null 2>&1
  rc=$?

  if [ "$rc" != "$expect_rc" ]; then
    echo "FAIL  $desc: exit $rc, erwartet $expect_rc"
    fail=1
    rm -f "$out" "$out.summary"
    return
  fi
  if [ "$expect_rc" != "0" ]; then
    echo "ok    $desc (abgelehnt, exit $rc)"
    rm -f "$out" "$out.summary"
    return
  fi

  local got_tag got_ver got_code got_pub got_ctag got_crel
  got_tag="$(sed -n 's/^tag=//p' "$out")"
  got_ver="$(sed -n 's/^version=//p' "$out")"
  got_code="$(sed -n 's/^version_code=//p' "$out")"
  got_pub="$(sed -n 's/^publish=//p' "$out")"
  got_ctag="$(sed -n 's/^create_tag=//p' "$out")"
  got_crel="$(sed -n 's/^create_release=//p' "$out")"
  if [ "$got_ctag" != "${EXPECT_CREATE_TAG:-false}" ] || \
     [ "$got_crel" != "${EXPECT_CREATE_RELEASE:-false}" ]; then
    echo "FAIL  $desc: create_tag=$got_ctag create_release=$got_crel" \
         "(erwartet ${EXPECT_CREATE_TAG:-false}/${EXPECT_CREATE_RELEASE:-false})"
    fail=1
  fi

  if [ "$got_tag" != "$expect_tag" ] || [ "$got_ver" != "$expect_version" ] ||
     [ "$got_code" != "$expect_code" ] || [ "$got_pub" != "$expect_pub" ]; then
    echo "FAIL  $desc: tag=$got_tag version=$got_ver code=$got_code publish=$got_pub" \
         "(erwartet $expect_tag/$expect_version/$expect_code/$expect_pub)"
    fail=1
  else
    echo "ok    $desc -> $got_tag version=$got_ver versionCode=$got_code publish=$got_pub"
  fi
  rm -f "$out" "$out.summary"
}

SHA_MAIN=bbbb1111
SHA_OLD=cccc2222
SHA_TAGSHA=dddd3333

# --- Push auf main: das ist der Fall, den der Nutzer gesehen hat -------
EXPECT_CREATE_TAG=true EXPECT_CREATE_RELEASE=true \
  REF_TYPE=branch HEAD_SHA=$SHA_MAIN TAG_LIST="" \
  run_case "main ohne Tag -> v1.0.0"  push main "" "" 0 v1.0.0 1.0.0 10000 true
EXPECT_CREATE_TAG=true EXPECT_CREATE_RELEASE=true \
  REF_TYPE=branch HEAD_SHA=$SHA_MAIN TAG_LIST="v0.0.0-ci.99 dddd0000" \
  run_case "Dry-Run-Tag zaehlt nicht" push main "" "" 0 v1.0.0 1.0.0 10000 true
EXPECT_CREATE_TAG=true EXPECT_CREATE_RELEASE=true \
  REF_TYPE=branch HEAD_SHA=$SHA_MAIN TAG_LIST="v1.0.0 $SHA_TAGSHA" \
  run_case "main-Push auf neuem Commit -> v1.0.1" push main "" "" 0 v1.0.1 1.0.1 10001 true
EXPECT_CREATE_TAG=true EXPECT_CREATE_RELEASE=true \
  REF_TYPE=branch HEAD_SHA=$SHA_MAIN TAG_LIST="v1.0.9 $SHA_OLD
v1.2.0 $SHA_OLD" \
  run_case "hoechstes Tag gewinnt, kein String-Vergleich" push main "" "" 0 v1.2.1 1.2.1 10201 true
EXPECT_CREATE_TAG=false EXPECT_CREATE_RELEASE=true \
  REF_TYPE=branch HEAD_SHA=$SHA_TAGSHA TAG_LIST="v1.0.0 $SHA_TAGSHA" \
  run_case "Tag steht schon auf main -> Release updaten" push main "" "" 0 v1.0.0 1.0.0 10000 true

run_case "Tag-Push v1.2.3"          push            v1.2.3 ""     ""      0 v1.2.3 1.2.3 10203 true
run_case "Dispatch mit Tag+publish"  workflow_dispatch main v1.2.3 true  0 v1.2.3 1.2.3 10203 true
run_case "Dispatch ohne Tag (Dry-Run)" workflow_dispatch main ""    false 0 v0.0.0-ci.4242 0.0.0-ci.4242 1 false
run_case "Dispatch mit Unsinn-Tag"   workflow_dispatch main "lol" false 1
run_case "Dispatch publish ohne Tag" workflow_dispatch main ""    true  0 v0.0.0-ci.4242 0.0.0-ci.4242 1 false

if [ "$fail" != "0" ]; then
  echo "RELEASE-VERSION-TEST: FEHLGESCHLAGEN"
  exit 1
fi
echo "RELEASE-VERSION-TEST: alle Faelle bestanden"