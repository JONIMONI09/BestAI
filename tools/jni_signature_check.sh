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
# Usage: bash tools/jni_signature_check.sh [Bridge.kt] [bridge.cpp]
#   bash tools/jni_signature_check.sh
#   bash tools/jni_signature_check.sh \
#     android/app/src/main/java/dev/hydrastone/LlamaBridge.kt \
#     android/app/src/main/cpp/llama_jni.cpp
# The optional arguments exist for the negative control: point the script
# at a bridge that declares a method the C side does not implement and it
# must fail (exit 1).
set -uo pipefail

KT="${1:-android/app/src/main/java/dev/hydrastone/HydraBridge.kt}"
JNI_C="${2:-android/app/src/main/cpp/hydra_jni.c}"

# The Kotlin `object` is named after its file in every bridge in this repo
# (HydraBridge.kt -> object HydraBridge, LlamaBridge.kt -> object LlamaBridge),
# so the class name for the generated stub comes from the file name rather than
# being hard-coded. That is what lets the same gate check the llama.cpp bridge
# as well, instead of shipping a second set of native methods with nothing
# verifying their descriptors.
KTCLASS="$(basename "$KT" .kt)"

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

# The return type is OPTIONAL: Kotlin writes `external fun cancel()` for a
# Unit-returning method. A pattern that required ": Type" would skip every
# such method, and a native method that nothing checks is a native method
# that can drift away from its C implementation unnoticed.
fns = re.findall(r"external\s+fun\s+(\w+)\s*\(([^)]*)\)\s*(?::\s*([\w.<>]+))?", src, re.S)
if not fns:
    sys.exit("no external fun declarations found in " + kt_path)

JMAP = {
    "String": "String",
    "Int": "int",
    "IntArray": "int[]",
    "Boolean": "boolean",
    "Long": "long",
    "Double": "double",
    "Float": "float",
    "Unit": "void",
    "Callback": "dev.hydrastone.%s.Callback" % cls,
}

# The Callback interface must mirror the Kotlin declaration, because the C
# bridge looks the method up BY NAME AND SIGNATURE
# (GetMethodID(..., "onTokens", "([IZ)V")). A stub with a stale onToken
# would still pass the native-method check while the real bridge could
# never find its callback at runtime.
CB_DECL = re.search(r"interface\s+Callback\s*\{(.*?)\}", src, re.S)
if not CB_DECL:
    sys.exit("no Callback interface found in " + kt_path)
cb_methods = []
for mm in re.finditer(r"fun\s+(\w+)\s*\(([^)]*)\)\s*(?::\s*([\w.<>\[\]]+))?", CB_DECL.group(1)):
    name, params = mm.group(1), mm.group(2)
    # Kotlin laesst den Rueckgabetyp bei Unit weg ("fun f(): Unit" und
    # "fun f()" sind dasselbe) - der Stub braucht in beiden Faellen void.
    ret = (mm.group(3) or "Unit").strip()
    jret = {"Unit": "void", "Int": "int", "Boolean": "boolean", "String": "String"}.get(ret)
    if jret is None:
        sys.exit("unsupported callback return type: " + ret)
    ps = []
    for p in (x.strip() for x in params.split(",")):
        if not p:
            continue
        pname, ptype = (x.strip() for x in p.split(":"))
        ps.append("%s %s" % (JMAP.get(ptype, "?"), pname))
    cb_methods.append("void %s(%s);" % (name, ", ".join(ps)))
if not cb_methods:
    sys.exit("Callback interface has no methods in " + kt_path)

out = ["package dev.hydrastone;", "",
       "/** Generated from %s - do not edit. */" % kt_path,
       "public final class %s {" % cls,
       "    public interface Callback { %s }" % " ".join(cb_methods)]
for name, params, ret in fns:
    ret_j = JMAP.get((ret or "Unit").strip())
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
    # -w matters: a plain substring search is satisfied by
    # Java_..._LlamaBridge_unloadXX when the check is for
    # Java_..._LlamaBridge_unload, so renaming a native method out of the way
    # would pass. -w anchors on non-word characters at both ends.
    if ! grep -qw "$sym" "$JNI_C"; then
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

# ---- 5: the callback lookup in C must match the Kotlin interface -----
# The native method signature check above says nothing about the callback:
# the C bridge looks the method up at runtime with GetMethodID(name, sig).
# If Kotlin declares onToken(int,int) and C calls onTokens(int[],boolean),
# every native signature check still passes while the app can never
# receive a single token. So the JVM descriptor of each callback method is
# derived from the generated stub and must appear in hydra_jni.c.
cb_sig="$(python3 - "$WORK/dev/hydrastone/$KTCLASS.java" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r"interface\s+Callback\s*\{(.*?)\}", src, re.S)
if not m:
    sys.exit("no Callback interface in the generated stub")
body = m.group(1)
out = []
for mm in re.finditer(r"void\s+(\w+)\s*\(([^)]*)\)\s*;", body):
    name = mm.group(1)
    # The generated stub writes bare `String` (it has no java.lang import) for
    # some bridges and the qualified form for others, so both are accepted.
    jt = {"int": "I", "int[]": "[I", "boolean": "Z", "long": "J", "float": "F",
          "String": "Ljava/lang/String;", "java.lang.String": "Ljava/lang/String;"}
    params = []
    for p in (x.strip() for x in mm.group(2).split(",")):
        if not p:
            continue
        jtype = p.rsplit(" ", 1)[0]
        if jtype not in jt:
            sys.exit("unsupported callback param type: " + jtype)
        params.append(jt[jtype])
    out.append("%s(%s)V" % (name, "".join(params)))
print(" ".join(out))
PY
)" || { echo "FAIL: could not derive the callback descriptor"; exit 1; }

echo "--- callback descriptor ---"
echo "derived: $cb_sig"
for sig in $cb_sig; do
  name="${sig%%(*}"
  # Die JVM-Signatur ist "(params)return" - die oeffnende Klammer gehoert
  # dazu und wird hier wieder ergaenzt.
  desc="(${sig#*(}"
  # GetMethodID takes name and descriptor as two separate strings, and the
  # descriptor contains [ and ] which would open a character class in a
  # regex (grep: "Unmatched ["), so both greps are fixed-string.
  if ! grep -qF "\"$name\"" "$JNI_C"; then
    echo "FAIL: the C bridge never looks up the callback method $name"
    exit 1
  fi
  if ! grep -qF "\"$desc\"" "$JNI_C"; then
    echo "FAIL: the C bridge looks up $name with a different descriptor than $desc"
    exit 1
  fi
  echo "ok    callback $name looked up as $desc in $JNI_C"
done

if [ "$fail" != "0" ]; then
  echo "JNI-SIGNATURE-CHECK: FAILED"
  exit 1
fi
echo "JNI-SIGNATURE-CHECK: $checked native method(s) verified against $JNI_C"