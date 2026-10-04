#!/usr/bin/env bash
# Download the small GGUF used to exercise the llama.cpp engine.
#
# WHY A SCRIPT AND NOT A COMMITTED FILE
# -------------------------------------
# The model is ~100 MB. Committing it would bloat the repository and the
# history permanently, and the licence of the WEIGHTS is not the repository's
# licence. So the weights are fetched on demand, into a directory outside the
# tree, and the licence is recorded next to them.
#
# WHAT THIS DOES NOT DO
# ---------------------
# It does not claim the model will load. It downloads a file and checks its
# size. Whether llama.cpp accepts it is a question for the device, and no
# speed, memory or quality claim is derived from it anywhere.
#
# Usage:
#   bash tools/fetch_test_model.sh [target-directory]
#
# Default target: $HYDRA_MODEL_DIR, else /tmp/hydra-models.
set -uo pipefail

MODEL_REPO="bartowski/SmolLM2-135M-Instruct-GGUF"
MODEL_FILE="SmolLM2-135M-Instruct-Q4_K_M.gguf"
MODEL_URL="https://huggingface.co/${MODEL_REPO}/resolve/main/${MODEL_FILE}"
# Measured 2026-10-03 with `curl -sIL`, which reports x-linked-size: 105454432.
# Not the ~70-100 MB the task brief guessed; the real file is slightly larger.
MODEL_BYTES=105454432

DEST_DIR="${1:-${HYDRA_MODEL_DIR:-/tmp/hydra-models}}"
DEST="${DEST_DIR}/${MODEL_FILE}"

say()  { printf '%s\n' "$*"; }
fail() { printf 'FETCH-TEST-MODEL: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null || fail "curl is required"

mkdir -p "$DEST_DIR" || fail "cannot create $DEST_DIR"

if [ -f "$DEST" ]; then
  have="$(wc -c < "$DEST")"
  if [ "$have" -eq "$MODEL_BYTES" ]; then
    say "already present: $DEST ($have bytes)"
  else
    fail "$DEST is $have bytes, expected $MODEL_BYTES - delete it and retry"
  fi
else
  say "downloading $MODEL_URL"
  say "  repository : $MODEL_REPO"
  say "  file       : $MODEL_FILE"
  say "  expected   : $MODEL_BYTES bytes"
  # -L follows the CDN redirect; --fail turns an HTTP error into a non-zero exit
  # instead of a 29-byte error page saved as if it were a model.
  curl -fL --retry 3 --retry-delay 2 -o "$DEST.part" "$MODEL_URL" \
    || fail "download failed (no partial file kept)"
  got="$(wc -c < "$DEST.part")"
  [ "$got" -eq "$MODEL_BYTES" ] || {
    rm -f "$DEST.part"
    fail "downloaded $got bytes, expected $MODEL_BYTES - refusing to keep it"
  }
  mv "$DEST.part" "$DEST"
  say "saved $DEST ($got bytes)"
fi

# Licences, recorded beside the weights because they govern the weights, not
# this repository. Verified 2026-10-03 via the HuggingFace model API, which
# reports apache-2.0 for both the GGUF repo and its base model.
cat > "${DEST_DIR}/LICENCE.txt" <<'EOF'
Third-party components fetched by tools/fetch_test_model.sh
=========================================================

1. Model weights
   file     : SmolLM2-135M-Instruct-Q4_K_M.gguf
   source   : https://huggingface.co/bartowski/SmolLM2-135M-Instruct-GGUF
   base     : https://huggingface.co/HuggingFaceTB/SmolLM2-135M-Instruct
   licence  : Apache-2.0  (reported by the HuggingFace model API for both the
               quantised repository and the base model; checked 2026-10-03)
   size     : 105454432 bytes

2. llama.cpp, linked into the APK as libllama.so / libllama_jni.so
   source   : https://github.com/ggml-org/llama.cpp
   tag      : v0.5.0
   commit   : 7fe450e19305b828c199d602c23a8337aaa1f03b
   licence  : MIT, "Copyright (c) 2023-2026 The ggml authors"
   path     : android/app/src/main/cpp/llama (git submodule)

The Hydra-Stone repository itself is licensed separately; see LICENSE.
The model weights are NOT part of this repository and must not be committed.
EOF
say "licence recorded in ${DEST_DIR}/LICENCE.txt"

say ""
say "Next: push it to the device and pick it with SAF import:"
say "  adb push \"$DEST\" /sdcard/Download/"
say "The app reads .gguf through llama.cpp and .hydra through the v1 engine."
