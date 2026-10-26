#!/usr/bin/env python3
"""Compose scroll-container crash guard.

WHY
---
Compose throws ``IllegalStateException("Vertically scrollable component was
measured with an infinity maximum height constraints ...")`` when a
vertically scrollable node (a ``Modifier.verticalScroll`` container, or a
``LazyColumn``) is measured with an infinite max height. Two source shapes
produce that:

1. LEXICAL nesting - a scrollable sits in the body of another same-axis
   scrollable in the same scope:

       Column(Modifier.verticalScroll(...)) {
           LazyColumn { ... }            // infinite max height -> throws

2. LAMBDA nesting - a composable lambda parameter is invoked inside a
   scrollable container, and its definition at the call site carries
   scrollable content. This is the exact 2026-10-26 Hydra-Stone crash
   (android 15 / arm64-v8a): ``ChatTab`` passed
   ``header = { Column(Modifier.verticalScroll(...)) { ... } }`` into
   ``AdaptiveHeaderSplit``, which invoked ``header()`` inside ITS OWN
   ``Column(...verticalScroll)``. The crash is invisible to a lexical
   scan of the split alone - the dangerous pair crosses a composable
   boundary - so detector 2 cross-references the call site and the
   lambda definition within the same file.

An instrumented Compose UI test would be the ideal detector, but this
repository's CI builds the APK without an emulator, so the guard is
static. A horizontal scrollable conflicts only with a horizontal inner
scrollable (infinite max WIDTH), a vertical one only with a vertical
inner scrollable (infinite max HEIGHT); cross-axis nesting is legal.

COMMENTS AND STRINGS ARE MASKED FIRST, so prose mentioning
"verticalScroll" (including this module's own documentation) can never
trip the guard.

USAGE
-----
    python3 tools/compose_scroll_guard.py FILE...

Exit 0 when nothing is found, 1 when at least one finding exists (or on
I/O error). ``tools/compose_scroll_check.sh`` is the gate wrapper with
the negative controls; run THAT, not this directly.
"""

import re
import sys

# Scrollable kinds. "axis" decides which pairs conflict: a vertical
# scrollable may only nest inside a horizontal one, and vice versa.
SCROLL_MODIFIER_RE = re.compile(r"(?<!\w)(verticalScroll|horizontalScroll)\s*\(")
# LazyColumn/LazyRow are called with a trailing lambda ("LazyColumn {")
# as often as with parens, so accept either as the call marker.
LAZY_CALL_RE = re.compile(r"\b(LazyColumn|LazyRow)\s*[\({]")
# A composable lambda parameter: "header: @Composable () -> Unit" (any
# function type with any leading annotations).
LAMBDA_PARAM_RE = re.compile(
    r"\b(\w+)\s*:\s*(?:@\w+\s+)*\([^()]*\)\s*->\s*[\w.]+")
LAMBDA_DEF_RE = re.compile(r"\b(\w+)\s*=\s*\{")


def mask_comments_and_strings(src: str) -> str:
    """Return src with comment bodies and string/char literal bodies
    replaced by spaces. Length and structure are preserved one-for-one,
    so index arithmetic on the masked text is valid."""
    out = list(src)
    i, n = 0, len(src)

    def blank(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = src[i]
        # line comment
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j == -1 else j
            blank(i, j)
            i = j
            continue
        # block comment (incl. KDoc)
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            blank(i, j)
            i = j
            continue
        # triple-quoted string
        if src.startswith('"""', i):
            j = src.find('"""', i + 3)
            j = n if j == -1 else j + 3
            blank(i + 3, j)
            i = j
            continue
        # regular string
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    j += 1
                j += 1
            blank(i + 1, min(j + 1, n) - 1)
            i = min(j + 1, n)
            continue
        # char literal
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                if src[j] == "\\":
                    j += 1
                j += 1
            blank(i + 1, min(j + 1, n) - 1)
            i = min(j + 1, n)
            continue
        i += 1
    return "".join(out)


def build_tables(text: str):
    """Brace/paren tables over the masked text.

    match_brace[open_pos] = close_pos; block_at[pos] = open_pos of the
    innermost brace open at pos (-1 at file root); same for parens.
    """
    n = len(text)
    match_brace = {}
    match_paren = {}
    stack = []
    pstack = []
    for i, ch in enumerate(text):
        if ch == "{":
            stack.append(i)
        elif ch == "}":
            if stack:
                o = stack.pop()
                match_brace[o] = i
        if ch == "(":
            pstack.append(i)
        elif ch == ")":
            if pstack:
                o = pstack.pop()
                match_paren[o] = i
    block_at = [-1] * n
    paren_at = [-1] * n
    stack = []
    pstack = []
    for i, ch in enumerate(text):
        block_at[i] = stack[-1] if stack else -1
        paren_at[i] = pstack[-1] if pstack else -1
        if ch == "{":
            stack.append(i)
        elif ch == "}":
            if stack:
                stack.pop()
        if ch == "(":
            pstack.append(i)
        elif ch == ")":
            if pstack:
                pstack.pop()
    return match_brace, block_at, match_paren, paren_at


def find_scrollables(text: str, block_at: list):
    """Return (pos, axis, label, block_open) for every scrollable
    occurrence; block_open is the innermost brace enclosing it."""
    occ = []
    for m in SCROLL_MODIFIER_RE.finditer(text):
        axis = "vertical" if m.group(1) == "verticalScroll" else "horizontal"
        occ.append((m.start(), axis, m.group(1) + "()", block_at[m.start()]))
    for m in LAZY_CALL_RE.finditer(text):
        axis = "vertical" if m.group(1) == "LazyColumn" else "horizontal"
        occ.append((m.start(), axis, m.group(1), block_at[m.start()]))
    return occ


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def detector_lexical(text: str, path: str, match_brace: dict,
                     scrollables: list):
    """Detector 1: a same-axis scrollable whose region is strictly inside
    another same-axis scrollable's region. Siblings (same region) are
    legal and stay unflagged."""
    findings = []
    for i, (pos_i, axis_i, label_i, r_i) in enumerate(scrollables):
        if r_i == -1:
            continue
        r_i_end = match_brace.get(r_i, -1)
        if r_i_end == -1:
            continue
        for j, (pos_j, axis_j, label_j, r_j) in enumerate(scrollables):
            if i == j or axis_i != axis_j or r_j == -1:
                continue
            r_j_end = match_brace.get(r_j, -1)
            if r_j_end == -1:
                continue
            if r_i < r_j < r_j_end < r_i_end:
                findings.append(
                    f"{path}:{line_of(text, pos_j)}: nested scrollable: "
                    f"{label_j} is inside the region of {label_i} "
                    f"({path}:{line_of(text, pos_i)}); the inner scrollable "
                    f"is measured with an infinite "
                    f"{'max height' if axis_i == 'vertical' else 'max width'}"
                    " and Compose throws at measure time")
    return findings


def scroll_content_blocks(text: str, match_brace: dict,
                          match_paren: dict, scrollables: list):
    """For each scroll MODIFIER occurrence, the content block of the
    composable call it modifies: the trailing "{...}" after that call's
    closing paren, if the call has one. Returns (mod_pos, block_open)."""
    blocks = []
    for pos, axis, label, r in scrollables:
        if not label.endswith("()"):
            continue  # lazy call: its own lambda is handled per-call below
        open_paren = -1
        # nearest enclosing paren at the modifier's start
        depth = 0
        for k in range(pos - 1, -1, -1):
            if text[k] == ")":
                depth += 1
            elif text[k] == "(":
                if depth == 0:
                    open_paren = k
                    break
                depth -= 1
        if open_paren == -1:
            continue
        close_paren = match_paren.get(open_paren, -1)
        if close_paren == -1:
            continue
        k = close_paren + 1
        while k < len(text) and text[k] in " \t\r\n":
            k += 1
        if k < len(text) and text[k] == "{":
            blocks.append((pos, k, match_brace.get(k, -1)))
    return blocks


def detector_lambda(text: str, path: str, match_brace: dict,
                    match_paren: dict, scrollables: list):
    """Detector 2: a composable lambda parameter invoked inside a
    scrollable container whose definition (same file) carries scrollable
    content - the 2026-10-26 crash shape."""
    findings = []
    content_blocks = scroll_content_blocks(text, match_brace, match_paren,
                                           scrollables)
    for mod_pos, block_open, block_end in content_blocks:
        if block_end == -1:
            continue
        # the function whose body contains this content block owns the
        # lambda parameters
        owner = _enclosing_function(text, block_open)
        if owner is None:
            continue
        func_name, func_open, func_close = owner
        params = set()
        header = None
        for hm in re.finditer(r"\bfun\s+" + re.escape(func_name) + r"\b",
                              text):
            if hm.start() < func_open:
                header = hm  # keep the last one before the body
        if header is None:
            continue
        sig_region = text[header.start():func_open]
        for m in LAMBDA_PARAM_RE.finditer(sig_region):
            params.add(m.group(1))
        if not params:
            continue
        # call sites of those lambdas inside this content block
        for name in params:
            call_re = re.compile(r"\b" + re.escape(name) + r"\s*\(")
            for cm in call_re.finditer(text, block_open + 1, block_end):
                # find the lambda's definition in the same file
                for dm in LAMBDA_DEF_RE.finditer(text):
                    if dm.group(1) != name:
                        continue
                    def_open = dm.end() - 1
                    def_end = match_brace.get(def_open, -1)
                    if def_end == -1:
                        continue
                    # the definition must be a composable lambda argument
                    # (not e.g. a local "x = { ... }" map builder); accept
                    # any lambda assignment - a scrollable inside any of
                    # them invoked from a scrollable is the crash shape
                    inner = [s for s in scrollables
                             if s[3] != -1 and def_open < s[0] < def_end]
                    if not inner:
                        continue
                    # same-axis check against the owning scrollable
                    own_axis = ("vertical" if "verticalScroll" in
                                _label_of(text, mod_pos) else "horizontal")
                    same = [s for s in inner if s[1] == own_axis]
                    if not same:
                        continue
                    findings.append(
                        f"{path}:{line_of(text, cm.start())}: composable "
                        f"lambda '{name}()' is invoked inside a "
                        f"{own_axis} scrollable (defined at "
                        f"{path}:{line_of(text, mod_pos)}) and its "
                        f"definition ({path}:{line_of(text, dm.start())}) "
                        f"carries scrollable content; the content is "
                        f"measured with an infinite "
                        f"{'max height' if own_axis == 'vertical' else 'max width'}"
                        " and Compose throws at measure time")
    return findings


def _label_of(text: str, pos: int) -> str:
    m = SCROLL_MODIFIER_RE.match(text, pos)
    return m.group(0) if m else ""


def _enclosing_function(text: str, pos: int):
    """(name, body_open, body_close) of the top-level 'fun' whose body
    block contains pos, or None. Walks function headers backwards."""
    # candidate: the nearest 'fun' before pos whose body brace encloses pos
    candidates = [m for m in re.finditer(r"\bfun\s+([A-Za-z_]\w*)", text)
                  if m.start() < pos]
    for m in reversed(candidates):
        # find the body '{' of this function: the first top-level brace
        # after the header, skipping the parameter paren list
        k = m.end()
        depth_paren = 0
        body_open = -1
        while k < len(text):
            ch = text[k]
            if ch == "(":
                depth_paren += 1
            elif ch == ")":
                if depth_paren > 0:
                    depth_paren -= 1
            elif ch == "{":
                # a brace at paren depth 0 after the header is the body;
                # lambdas in default arguments sit inside the parens and
                # are skipped by the depth check
                if depth_paren == 0:
                    body_open = k
                    break
            elif ch == ";" and depth_paren == 0:
                break  # parameterless "fun name" without a body
            k += 1
        if body_open == -1:
            continue
        # the '{' must be the function body: nothing else (no other fun)
        # opened a block between the header and this brace at depth 0
        body_close = None
        depth = 0
        k = body_open
        while k < len(text):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    body_close = k
                    break
            k += 1
        if body_close is None or not (body_open < pos < body_close):
            continue
        # ensure no other function body starts between body_open and pos
        inner = [c for c in re.finditer(r"\bfun\s+\w+", text)
                 if body_open < c.start() < pos]
        # nested functions are legal; the NEAREST enclosing one wins, and
        # 'candidates' is scanned from the closest backwards, so the first
        # hit is the nearest. A nested lambda containing pos would have
        # been reported by an earlier candidate, so this is the owner.
        return (m.group(1), body_open, body_close)
    return None


def scan_source(src: str, path: str):
    """Scan one file with both detectors. Return finding strings."""
    masked = mask_comments_and_strings(src)
    match_brace, block_at, match_paren, _paren_at = build_tables(masked)
    scrollables = find_scrollables(masked, block_at)
    return (detector_lexical(masked, path, match_brace, scrollables)
            + detector_lambda(masked, path, match_brace, match_paren,
                              scrollables))


def main(argv):
    if len(argv) < 2:
        print("usage: compose_scroll_guard.py FILE...", file=sys.stderr)
        return 2
    total = 0
    for path in argv[1:]:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
        except OSError as exc:
            print(f"FAIL {path}: {exc}")
            return 1
        for f in scan_source(src, path):
            print(f"FAIL {f}")
            total += 1
    if total == 0:
        print(f"ok   no nested scrollable containers in {len(argv) - 1} file(s)")
        return 0
    print(f"FAIL {total} nested scrollable container(s) found")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
