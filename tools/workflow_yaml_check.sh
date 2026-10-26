#!/usr/bin/env bash
# Workflow YAML validation gate (regression for the 2026-10-26 PR #53
# red run: a duplicate `key:` in the actions/cache save mapping of
# .github/workflows/lint.yml).
#
# Why this exists: PyYAML - and most YAML tooling - silently accepts
# duplicate mapping keys (last one wins), so a plain `yaml.safe_load`
# check reports "ok" for a workflow file that GitHub's workflow
# compiler REJECTS (the run is created with zero jobs and fails in the
# same second; no job log exists to read). The GitHub compiler is
# strictly stricter than the YAML spec. The only reliable local
# substitute is a duplicate-key scan plus a full parse.
#
# Behaviour:
#   * every .github/**/*.yml|yaml is parsed;
#   * any duplicate key at any level fails the gate (R25: the fixture
#     tools/fixtures/workflow_yaml_bad.yml contains one and must fail);
#   * PyYAML is used when importable (it ships on the GitHub runners);
#     without it a stdlib indent-based duplicate-key scan is applied -
#     it catches the workflow-class duplicates this gate was born for,
#     but it cannot detect general syntax errors, so it says so.
set -u
cd "$(dirname "$0")/.."

python3 - <<'PY'
import glob, os, sys

paths = sorted(glob.glob(".github/**/*.yml", recursive=True)
               + glob.glob(".github/**/*.yaml", recursive=True))
if not paths:
    print("FAIL workflow_yaml_check: no workflow files found under .github/")
    sys.exit(1)

def stdlib_dup_scan(path):
    """Detect `key:` duplicates in the workflow-class of YAML (stdlib
    only; it is NOT a YAML parser and cannot see syntax errors).

    Model: every mapping key remembers (key -> (indent, line)). Any
    line (mapping key or sequence dash) at indent I clears the history
    of all keys recorded at a DEEPER indent - a shallower line starts a
    new context for everything below it, which is what separates the
    `run:` of one step from the `run:` of the next. A key is therefore
    a duplicate when it reappears at the same indent without any
    shallower line in between - exactly the shape of the duplicate
    `key:` under `with:` that broke the PR #53 lint.yml run.

    Block scalars (`run: |`) are skipped: their indented content is not
    YAML structure (embedded shell/python would otherwise look like
    keys and produce false positives)."""
    errors = []
    seen = {}          # key name -> (indent, first line)
    block_owner = None  # indent of the `key: |` line while inside its block
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.strip().startswith("#"):
                continue
            indent = len(line) - len(line.lstrip(" "))
            if block_owner is not None:
                if indent <= block_owner:
                    block_owner = None  # block ended; process this line
                else:
                    continue  # block scalar content, not structure
            stripped = line.strip()
            if stripped.startswith("- "):
                for k in [k for k, (i, _) in seen.items() if i > indent]:
                    del seen[k]
                rest = stripped[2:].strip()
                if ":" in rest:  # the item's first key is on the dash line
                    key = rest.split(":", 1)[0].strip().strip('"').strip("'")
                    if key:
                        eff = indent + 2
                        if key in seen and seen[key][0] == eff:
                            errors.append(f"{path}:{lineno}: duplicate key "
                                          f"{key!r} (first at line "
                                          f"{seen[key][1]})")
                        else:
                            seen[key] = (eff, lineno)
                continue
            if ":" not in stripped:
                continue  # not a mapping entry
            for k in [k for k, (i, _) in seen.items() if i > indent]:
                del seen[k]
            key = stripped.split(":", 1)[0].strip().strip('"').strip("'")
            if not key:
                continue
            value = stripped.split(":", 1)[1].strip()
            if key in seen and seen[key][0] == indent:
                errors.append(f"{path}:{lineno}: duplicate key {key!r} "
                              f"(first at line {seen[key][1]})")
            else:
                seen[key] = (indent, lineno)
            if value in ("|", ">", "|-", ">-", "|+", ">+"):
                block_owner = indent
    return errors

try:
    import yaml  # noqa: F401
except ImportError:
    yaml = None

def make_no_dup_loader(base):
    """A SafeLoader subclass that rejects duplicate mapping keys - the
    check GitHub's workflow compiler makes and plain yaml.safe_load
    does not (it silently keeps the last one)."""
    class NoDup(base):
        pass

    def no_dup(loader, node, deep=False):
        mapping = {}
        for k, v in node.value:
            key = loader.construct_object(k, deep=deep)
            if key in mapping:
                raise ValueError(f"duplicate key {key!r} at line "
                                 f"{k.start_mark.line + 1}")
            mapping[key] = loader.construct_object(v, deep=deep)
        return mapping

    NoDup.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
                          no_dup)
    return NoDup

failed = 0
if yaml is not None:
    NoDup = make_no_dup_loader(yaml.SafeLoader)

    def no_dup(loader, node, deep=False):
        mapping = {}
        for k, v in node.value:
            key = loader.construct_object(k, deep=deep)
            if key in mapping:
                raise ValueError(f"{p}:{k.start_mark.line + 1}: duplicate "
                                 f"key {key!r}")
            mapping[key] = loader.construct_object(v, deep=deep)
        return mapping

    NoDup.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
                          no_dup)
    for p in paths:
        try:
            with open(p, encoding="utf-8") as fh:
                yaml.load(fh, Loader=NoDup)
            print(f"ok   {p}")
        except ValueError as exc:
            print(f"FAIL {exc}")
            failed += 1
        except yaml.YAMLError as exc:
            print(f"FAIL {p}: YAML syntax error: "
                  f"{str(exc).splitlines()[0]}")
            failed += 1
else:
    print("note PyYAML not importable - stdlib duplicate-key scan only "
          "(it cannot detect general syntax errors)")
    for p in paths:
        errs = stdlib_dup_scan(p)
        if errs:
            failed += 1
            for e in errs:
                print(f"FAIL {e}")
        else:
            print(f"ok   {p} (stdlib duplicate-key scan)")

# Negative control (R25): the fixture must be detected.
fixture = "tools/fixtures/workflow_yaml_bad.yml"
if not os.path.exists(fixture):
    print(f"FAIL {fixture} missing (the gate would be unproven, R25)")
    sys.exit(1)
if yaml is not None:
    NoDupFixture = make_no_dup_loader(yaml.SafeLoader)
    try:
        with open(fixture, encoding="utf-8") as fh:
            yaml.load(fh, Loader=NoDupFixture)
        print(f"FAIL {fixture}: the duplicate key was NOT detected "
              f"(detector is broken, R25)")
        failed += 1
    except ValueError:
        print(f"ok   {fixture}: duplicate key detected (negative control)")
else:
    errs = stdlib_dup_scan(fixture)
    if errs:
        print(f"ok   {fixture}: duplicate key detected (negative control)")
    else:
        print(f"FAIL {fixture}: the duplicate key was NOT detected "
              f"(detector is broken, R25)")
        failed += 1

if failed:
    print(f"FAIL workflow_yaml_check: {failed} failure(s)")
    sys.exit(1)
print("WORKFLOW-YAML-CHECK: ok "
      f"({len(paths)} workflow files parsed, negative control proven)")
PY
