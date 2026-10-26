#!/usr/bin/env python3
"""Issue verification and auto-close (2026 pattern, gh CLI based).

WHY
---
Features land in main and the issues describing them stay open, so the
tracker fills with implemented work. This tool is the other half of the
".github/issue-verifications.yml" mapping: for every listed issue it runs
the gate command(s) that PROVE the issue is implemented, and closes the
issue - with a comment linking the proving run - only when every gate
exits 0 on the commit the run was triggered by.

Design rules (web research 2024-2026: gh issue close is the maintained
standard, third-party close actions are deprecated; the "self-closing
alert" pattern from large CI setups only closes when the check behind
the close actually RAN and PASSED):

* a failing gate closes NOTHING (the issue stays open, and the failure
  is reported so the run is red, not silently skipped),
* an issue that is already closed or not found is skipped, not an error
  (re-runs after a manual close must not fail the pipeline),
* only issues listed in the mapping are ever touched - no bulk logic,
* the close comment always names the commit and the run, so a wrong
  auto-close is auditable and reopenable with context.

USAGE
-----
    python3 tools/issue_verify.py [--config .github/issue-verifications.yml]
                                  [--dry-run]

Exit 0 when every gate of every listed open issue passed (and the issue
was closed, unless --dry-run), 1 when any gate failed.
"""

import argparse
import os
import subprocess
import sys

DEFAULT_CONFIG = ".github/issue-verifications.yml"


def load_mapping(path):
    """Parse the YAML mapping with PyYAML when available, falling back
    to a tiny parser for the restricted shape used by this repo
    (two-level mapping: issue number -> {summary, gates: [cmd...]}).
    The fallback exists so the tool has zero hard dependencies; PyYAML
    is installed by the workflow before calling this script."""
    try:
        import yaml  # type: ignore
        with open(path, "r", encoding="utf-8") as fh:
            data = yaml.safe_load(fh) or {}
        issues = data.get("issues", {}) or {}
        out = {}
        for number, spec in issues.items():
            out[str(number)] = {
                "summary": spec.get("summary", ""),
                "gates": list(spec.get("gates", [])),
            }
        return out
    except ImportError:
        pass

    out = {}
    current = None
    in_gates = False
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if stripped == "issues:":
                continue
            if line.startswith('  "') and stripped.endswith(":"):
                # "52":  ->  52  (strip the colon FIRST, then the quotes)
                number = stripped.rstrip(":").strip().strip('"')
                current = str(number)
                out[current] = {"summary": "", "gates": []}
                in_gates = False
                continue
            if current is None:
                continue
            if stripped.startswith("summary:"):
                out[current]["summary"] = (stripped.split(":", 1)[1]
                                           .strip().strip('"').strip("'"))
                in_gates = False
            elif stripped == "gates:":
                in_gates = True
            elif in_gates and stripped.startswith("- "):
                cmd = stripped[2:].strip().strip('"').strip("'")
                out[current]["gates"].append(cmd)
    return out


def run_gate(cmd, log_path):
    """Run one gate command in a subshell, appending to log_path.
    Return (rc, output_tail)."""
    with open(log_path, "a", encoding="utf-8") as log:
        log.write(f"\n$ {cmd}\n")
        log.flush()
        proc = subprocess.run(
            ["bash", "-o", "pipefail", "-c", cmd],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        out = proc.stdout or ""
        log.write(out)
    return proc.returncode, out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    ap.add_argument("--dry-run", action="store_true",
                    help="run the gates but never call gh")
    args = ap.parse_args(argv[1:])

    if not os.path.exists(args.config):
        print(f"ok   no issue mapping at {args.config}; nothing to verify")
        return 0

    mapping = load_mapping(args.config)
    if not mapping:
        print(f"ok   {args.config} lists no issues; nothing to verify")
        return 0

    sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True).stdout.strip()
    run_url = os.environ.get("GITHUB_RUN_URL", "")
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    log_path = "/tmp/issue-verify.log"
    open(log_path, "w").close()

    failures = 0
    for number, spec in mapping.items():
        gates = spec["gates"]
        if not gates:
            print(f"FAIL issue #{number}: mapped but has no gates")
            failures += 1
            continue
        # state first: already-closed or missing issues are skipped
        state = subprocess.run(
            ["gh", "issue", "view", number,
             "--json", "state"],
            capture_output=True, text=True,
            env={**os.environ, "GH_TOKEN": os.environ.get("GH_TOKEN", "")},
        )
        if state.returncode != 0:
            print(f"ok   issue #{number}: not found or not accessible; skipped")
            continue
        if "CLOSED" in state.stdout:
            print(f"ok   issue #{number}: already closed; skipped")
            continue

        all_ok = True
        for cmd in gates:
            rc, tail = run_gate(cmd, log_path)
            name = cmd.splitlines()[0][:60]
            if rc == 0:
                print(f"ok   issue #{number}: gate passed: {name}")
            else:
                all_ok = False
                failures += 1
                print(f"FAIL issue #{number}: gate failed (rc={rc}): {name}")
                print("     " + "\n     ".join(
                    (tail or "<no output>").strip().splitlines()[-6:]))

        if not all_ok:
            continue  # the issue stays open; nothing is closed

        if args.dry_run:
            print(f"ok   issue #{number}: would be closed (dry run)")
            continue
        if not repo:
            print(f"FAIL issue #{number}: GITHUB_REPOSITORY not set; cannot close")
            failures += 1
            continue
        comment = (
            f"Automatically closed: every verification gate for this issue "
            f"passed on main@{sha}"
            + (f" (run: {run_url})" if run_url else "")
            + ".\n\n"
            f"{spec['summary']}\n\n"
            f"Gates:\n"
            + "\n".join(f"- `{g}`" for g in gates)
            + "\n\n"
            f"Reopen with context if this is wrong - the comment is the audit trail."
        )
        close = subprocess.run(
            ["gh", "issue", "close", number,
             "--comment", comment],
            capture_output=True, text=True,
            env={**os.environ, "GH_TOKEN": os.environ.get("GH_TOKEN", "")},
        )
        if close.returncode == 0:
            print(f"ok   issue #{number}: closed with the proving run linked")
        else:
            failures += 1
            print(f"FAIL issue #{number}: gh issue close failed: "
                  f"{(close.stderr or close.stdout).strip()[:200]}")

    if failures:
        print(f"FAIL {failures} gate(s) failed or close attempt(s) failed "
              f"(full log: {log_path})")
        return 1
    print("issue_verify: all listed open issues verified"
          + (" (dry run)" if args.dry_run else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
