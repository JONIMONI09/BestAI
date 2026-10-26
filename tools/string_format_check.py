#!/usr/bin/env python3
"""Gate: every format specifier in the Android string resources can be
satisfied by the arguments its Kotlin call site actually passes.

WHY
---
This gate exists because of a crash that only a running app could show.
R.plurals.log_running was declared with "%2$d" but was read with
getQuantityString(id, qty, qty) - one format argument. Android lint does not
check the argument count of an indexed specifier inside a plurals entry, so
the build stayed green, every unit test stayed green, and the app died on its
first frame with:

    java.util.MissingFormatArgumentException: Format specifier '%2$d'
        at dev.hydrastone.MainActivity.startInference(MainActivity.kt:361)

The compile is not the test. A string that cannot be formatted is a crash
that only appears when a user presses a button. rules.md R7.

WHAT IS CHECKED
---------------
Every <string> and <plurals> entry in res/values/strings.xml is read, and the
highest "%N$" index it uses is compared against the number of arguments its
Kotlin call sites actually pass:

  * <string>   a bare "%d" counts as index 1, because getString(id, a) binds
               it to a. An entry with no specifier is ignored.
  * <plurals>  Resources.getQuantityString(id, quantity, ...args) passes the
               quantity SEPARATELY, so "%1$d" is the first format argument and
               one trailing argument is the normal shape. A "%2$" or higher
               means the entry wants an argument the quantity does not supply.

Call sites are parsed as balanced parentheses across line breaks, because the
real call sites in this project are wrapped over several lines. An entry read
somewhere other than a literal getString(...) call - through a helper, or with
the resource id passed in - is reported as unverified rather than silently
passed.

WHAT THIS GATE DOES NOT COVER, STATED PLAINLY
---------------------------------------------
A <string> that is read with getString(R.string.x) and NO arguments is a legal
pattern in this project: the resolved text is a deferred format template that
is passed to String.format later (see MainActivity.reportModel). Nothing in
the call site distinguishes that from a genuine missing argument, so such an
entry is reported as unverified rather than failed. A <plurals> entry has no
such escape hatch - the quantity is the only implicit argument - so those are
checked strictly. That strict plurals rule is the one that catches the crash
this gate was written for.

Usage: python3 tools/string_format_check.py
Exit 0 when every entry can be formatted, 1 otherwise.

Negative control: tools/string_format_negative_test.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STRINGS = ROOT / "android/app/src/main/res/values/strings.xml"
KT_DIR = ROOT / "android/app/src/main/java/dev/hydrastone"

PASS = 0
FAIL = 0
UNVERIFIED = 0


def ok(msg: str) -> None:
    global PASS
    PASS += 1
    print(f"ok   {msg}")


def bad(msg: str, detail: str = "") -> None:
    global FAIL
    FAIL += 1
    print(f"FAIL {msg}")
    if detail:
        print(f"     {detail}")


def unverified(msg: str, detail: str = "") -> None:
    global UNVERIFIED
    UNVERIFIED += 1
    print(f"skip {msg}")
    if detail:
        print(f"     {detail}")


# --------------------------------------------------------------------------
# Kotlin call sites
# --------------------------------------------------------------------------

def split_top_level_args(inner: str) -> list[str]:
    """Split an argument list on commas that are not inside (), [] or {}."""
    args, depth, current = [], 0, ""
    for ch in inner:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
    tail = current.strip()
    if tail or args:
        args.append(tail)
    return [a for a in args if a != ""]


def find_calls(source: str, fn: str) -> list[list[str]]:
    """Every fn(...) call in the source, returned as a list of argument texts.

    The lookbehind only excludes identifier characters, NOT a dot: real call
    sites read ctx.getString(...) and ctx.resources.getQuantityString(...),
    and a gate that cannot see those would report every entry as "no call
    site found" and go green on anything.
    """
    calls = []
    for m in re.finditer(r"(?<![A-Za-z0-9_])" + fn + r"\s*\(", source):
        i = m.end()  # just past the '('
        depth = 1
        start = i
        while i < len(source) and depth > 0:
            if source[i] == "(":
                depth += 1
            elif source[i] == ")":
                depth -= 1
            i += 1
        if depth == 0:
            calls.append(split_top_level_args(source[start:i - 1]))
    return calls


RES_REF = re.compile(r"^R\.(string|plurals)\.([A-Za-z0-9_]+)$")


def collect_call_sites() -> tuple[dict, dict]:
    """Map resource name -> the argument counts its call sites pass."""
    strings: dict[str, set[int]] = {}
    plurals: dict[str, set[int]] = {}

    for kt in sorted(KT_DIR.rglob("*.kt")):
        source = kt.read_text(encoding="utf-8")
        for args in find_calls(source, "getString"):
            ref = RES_REF.match(args[0].strip()) if args else None
            if ref and ref.group(1) == "string":
                strings.setdefault(ref.group(2), set()).add(len(args) - 1)
        for args in find_calls(source, "getQuantityString"):
            ref = RES_REF.match(args[0].strip()) if args else None
            if ref and ref.group(1) == "plurals":
                # args = [id, quantity, *format_args]
                plurals.setdefault(ref.group(2), set()).add(max(0, len(args) - 2))

    return strings, plurals


# --------------------------------------------------------------------------
# Resource entries
# --------------------------------------------------------------------------

def resource_bodies() -> dict:
    xml = STRINGS.read_text(encoding="utf-8")
    bodies = {}
    for kind in ("string", "plurals"):
        for m in re.finditer(
            r"<%s name=\"([A-Za-z0-9_]+)\"[^>]*>(.*?)</%s>" % (kind, kind), xml, re.S
        ):
            bodies[(kind, m.group(1))] = m.group(2)
    return bodies


def highest_index(body: str) -> int:
    """How many format arguments this resource body consumes.

    An indexed specifier "%N$" binds argument N. A bare specifier "%d" takes
    the next free argument, so "value %d and %d" needs TWO arguments even
    though it contains no index at all - reading that as "one specifier" is
    how a gate like this one silently stops checking anything.
    """
    body = body.replace("%%", "")  # an escaped percent is not a specifier
    indexed = [int(n) for n in re.findall(r"%(\d+)\$", body)]
    # Bare specifiers: no index, no '%%'. A trailing type letter is what
    # separates them from the stray percent signs in ordinary prose.
    bare = re.findall(r"%[-#+ 0,(]*\d*(?:\.\d+)?[a-zA-Z]", body)
    return (max(indexed) if indexed else 0) + len(bare)


def main() -> int:
    if not STRINGS.is_file():
        bad("strings.xml exists", f"{STRINGS} is missing")
        print(f"\nSTRING-FORMAT: {PASS} passed, {FAIL} failed")
        return 1
    ok("strings.xml exists")

    strings, plurals = collect_call_sites()
    bodies = resource_bodies()
    if not bodies:
        bad("the resource file yields entries", "no <string>/<plurals> entry parsed")
        print(f"\nSTRING-FORMAT: {PASS} passed, {FAIL} failed")
        return 1

    checked = 0

    for (kind, name), body in sorted(bodies.items()):
        need = highest_index(body)
        if need == 0:
            continue
        checked += 1
        if kind == "plurals":
            label = f"plurals/{name}"
            have_any = plurals.get(name)
        else:
            label = f"string/{name}"
            have_any = strings.get(name)

        if have_any is None:
            unverified(
                f"{label}: needs {need} format argument(s), no literal call site found",
                "the id may be built or passed indirectly; this gate cannot check it",
            )
            continue

        # A <string> read with NO arguments is a legal pattern in this project:
        # the resolved text is a deferred format template handed to
        # String.format later (see reportModel). A <plurals> entry has no such
        # escape - the quantity is the only implicit argument, so "%1$" is the
        # first format argument and anything higher is wrong by construction.
        if kind == "string" and min(have_any) == 0:
            unverified(
                f"{label}: needs {need} format argument(s) but is read with none",
                "a zero-argument getString can be a deferred format template; "
                "whether it is later formatted cannot be decided here",
            )
            continue

        if any(n < need for n in have_any):
            bad(
                f"{label} is read with enough arguments",
                f"the resource uses %{need}$ but a call site passes "
                f"{min(have_any)} format argument(s); on a device this throws "
                f"MissingFormatArgumentException",
            )
            continue

        ok(f"{label}: needs {need} format argument(s), call sites pass "
           f"{sorted(have_any)}")

    if checked == 0:
        bad("the scan found format specifiers to check",
            "no <string>/<plurals> entry with a specifier was parsed")
    else:
        ok(f"parsed {checked} resource entries carrying format specifiers")

    print(f"\nSTRING-FORMAT: {PASS} passed, {FAIL} failed, {UNVERIFIED} unverified")
    if FAIL:
        print("STRING-FORMAT: FAIL")
        return 1
    print(f"STRING-FORMAT: ok ({checked} entries checked, "
          f"{UNVERIFIED} not statically checkable)")
    return 0


if __name__ == "__main__":
    sys.exit(main())