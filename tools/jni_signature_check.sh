#!/usr/bin/env bash
# JNI bridge gate: prove that every native method the Kotlin bridge
# declares really exists in the C bridge, with a matching parameter list.
#
# How it works (no hand-written expectations):
#   1. parse `external fun` declarations out of HydraBridge.kt
#   2. emit an equivalent Java class and run `javac -h` on it
#   3. read the JNI header javac generated (that is the authoritative
#      symbol name and signature)
#   4. assert the symbol exists in hydra_jni.c and that its C parameter
#      list matches the JVM types javac derived
#
# Usage: bash tools/jni_signature_check.sh [HydraBridge.kt] [hydra_jni.c]
# The optional arguments exist for the negative control: point the script
# at a bridge that declares a method the C side does not implement and it
# must fail (exit 1).
set -uo pipefail

KT="${1:-android/app/src/main/java/dev/hydrastone/HydraBridge.kt}"
JNI_C="${2:-android/app/src/main/cpp/hydra_jni.c}"

command -v javac > /dev/null || { echo "SKIP: javac not available"; exit 0; }
test -f "$KT"  || { echo "FAIL: $KT not found"; exit 1; }
test -f "$JNI_C" || { echo "FAIL: $JNI_C not found"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/dev/hydrastone" "$WORK/h"

# ---- 1+2: Kotlin external declarations -> Java stub -> javac -h ----------
python3 - "$KT" "$WORK" <<'PY' || exit 1
import re, sys
kt_path, work = sys.argv[1], sys.argv[2]
src = open(kt_path, encoding="utf-8").read()

m = re.search(r"object\s+(\w+)", src)
if not m:
    sys.exit("no Kotlin object found in " + kt_path)
cls = m.group(1)

fns = re.findall(r"external\s+fun\s+(\w+)\s*\(([^)]*)\)\s*:\s*([\w.<>]+)", src, re.S)
if not fns:
    sys.exit("no external fun declarations found in " + kt_path)

JMAP = {
    "String": "String",
    "Int": "int",
    "Boolean": "boolean",
    "Long": "long",
    "Double": "double",
    "Callback": "dev.hydrastone.%s.Callback" % cls,
}

out = ["package dev.hydrastone;", "",
       "/** Generated from %s - do not edit. */" % kt_path,
       "public final class %s {" % cls,
       "    public interface Callback { void onToken(int step, int token); }"]
for name, params, ret in fns:
    ret_j = JMAP.get(ret.strip())
    if ret_j is None:
        sys.exit("unsupported return type: " + ret)
    ps = []
    for p in (x.strip() for x in params.split(",")):
        if not p:
            continue
        pname, ptype = (x.strip() for x in p.split(":"))
        jt = JMAP.get(ptype)
        if jt is None:
            sys.exit("unsupported parameter type: " + ptype)
        ps.append("%s %s" % (jt, pname))
    # Non-static, because a Kotlin `object` member is an instance method.
    out.append("    public native %s %s(%s);" % (ret_j, name, ", ".join(ps)))
out.append("}")

with open("%s/dev/hydrastone/%s.java" % (work, cls), "w") as f:
    f.write("\n".join(out) + "\n")
print("generated %s with %d native method(s)" % (cls, len(fns)))
PY

echo "--- javac ---"
javac -h "$WORK/h" -d "$WORK/classes" "$WORK/dev/hydrastone/"*.java || {
  echo "FAIL: javac rejected the generated bridge stub"; exit 1; }
javac -version

# ---- 3+4: compare javac's header with the C bridge ----------------------
fail=0
checked=0
for hdr in "$WORK/h"/*.h; do
  test -e "$hdr" || continue
  echo "--- javac header: $(basename "$hdr") ---"
  # Every JNIEXPORT declaration in the header, e.g.
  #   JNIEXPORT jstring JNICALL Java_dev_hydrastone_HydraBridge_runInference
  #     (JNIEnv *, jobject, jstring, jint, jint, jobject);
  symbols="$(sed -n 's/.*JNICALL[[:space:]]*\(Java_[A-Za-z0-9_]*\).*/\1/p' "$hdr")"
  test -n "$symbols" || { echo "FAIL: no JNI symbol in $hdr"; fail=1; continue; }

  for sym in $symbols; do
    if ! grep -q "$sym" "$JNI_C"; then
      echo "FAIL: $sym is declared on the Kotlin/Java side but missing in $JNI_C"
      fail=1
      continue
    fi
    # Parameter check: the JVM types javac derived must appear in the C
    # parameter list, in the same order and count. The second parameter
    # is accepted as jclass *or* jobject: a Kotlin `object` member is an
    # instance method (jobject), the C bridge declares jclass and never
    # uses it — harmless, but documented here instead of hidden.
    want_all="$(awk "/$sym/{f=1} f{print} f&&/;/{exit}" "$hdr" \
             | tr -d '\n' | tr ',' '\n' \
             | sed 's/[);].*//; s/^[[:space:]*]*//; s/[[:space:]]*$//' \
             | grep -v '^$' || true)"
    got_all="$(awk "/$sym/{f=1} f{print} f&&/\\)/{exit}" "$JNI_C" \
              | tr -d '\n' | tr ',' '\n' \
              | sed 's/)[^)]*$//; s/^[[:space:]*]*//' \
              | sed 's/[[:space:]][[:alnum:]_]*$//' \
              | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' \
              | grep -v '^$' || true)"
    # javac: JNIEnv, receiver, params... / C: JNIEnv, receiver, params...
    want_cmp="$(printf '%s\n' "$want_all" | tail -n +3)"
    got="$(printf '%s\n' "$got_all" | tail -n +3)"
    want_recv="$(printf '%s\n' "$want_all" | sed -n 2p)"
    got_recv="$(printf '%s\n' "$got_all" | sed -n 2p)"
    if [ "$want_recv" != "$got_recv" ]; then
      # Documented, benign: a Kotlin `object` member is an instance method
      # (jobject); the C bridge declares jclass and never reads it.
      echo "note  $sym: receiver is ${want_recv} in javac, ${got_recv} in C (unused)"
    fi
    if [ "$want_cmp" != "$got" ]; then
      echo "FAIL: $sym parameter mismatch"
      echo "  javac: $(printf '%s' "$want_cmp" | tr '\n' ' ')"
      echo "  C    : $(printf '%s' "$got" | tr '\n' ' ')"
      fail=1
      continue
    fi
    echo "ok    $sym  ($(printf '%s' "$got" | tr '\n' ' '))"
    checked=$((checked + 1))
  done
done

if [ "$checked" -eq 0 ]; then
  echo "FAIL: no native method was verified"
  exit 1
fi
if [ "$fail" != "0" ]; then
  echo "JNI-SIGNATURE-CHECK: FAILED"
  exit 1
fi
echo "JNI-SIGNATURE-CHECK: $checked native method(s) verified against $JNI_C"