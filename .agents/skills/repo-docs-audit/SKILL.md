---
name: repo-docs-audit
description: "Audit and fix every Markdown file of the Hydra-Stone repo: find all .md files, catch stale paths, dead references, banned capability claims, language violations and broken skill frontmatter. Use when asked to improve/clean up docs, after any folder reorganization, or before a release."
---

# Repo Docs Audit — Hydra-Stone

The docs of this repo are a contract (owner rules: "Document
everything!!!", R6 "no claimed capabilities", R31 English
artifacts). This skill is the repeatable audit that keeps 28+ files
honest after reorganizations and rounds of work.

## When to use

- "Improve/clean up all .md files" (owner request).
- After ANY folder move (e.g. the 2026-10-26 `.agents/` reorg broke
  path references in 44 files).
- Before a release tag (docs must match the shipped state).

## Step 1 — Inventory (every folder, no exceptions)

```bash
find . -name "*.md" -not -path "./.git/*" -not -path "./llama.cpp/*" \
  -not -path "*/node_modules/*" | sort
```

`llama.cpp` is a pinned submodule — its docs are upstream's, never
edited here.

## Step 2 — Mechanical checks (run, don't eyeball)

```bash
# 1. Banned capability claims (the "no claimed capabilities" gate):
bash tools/docs_claim_check.sh

# 2. Workflow files still parse + lint (docs often embed them):
bash tools/workflow_yaml_check.sh && bash tools/workflow_lint_check.sh

# 3. String-format placeholders vs. format args:
python3 tools/string_format_check.py

# 4. Stale path references: grep every file that MOVES/DELETES for its
#    old path across the whole repo, then fix references:
grep -rn "OLD/PATH" --include="*.md" --include="*.yml" --include="*.sh" .
```

## Step 3 — Per-file review checklist

For every `.md`:

1. **Stale facts**: PR numbers ("PR #59 open"), commit shas, CI state,
   "pending"/"open" items that have since been resolved — update or
   mark superseded.
2. **Dead references**: links/paths to files that moved (especially
   after reorgs: `.agent_memory/`, root-level `rules.md`/`errors.md`/
   `session.md`/`Environment.md`/`CLAUDE.md` now live in `.agents/`;
   skills live in `.agents/skills/` with `.claude/skills` as a symlink).
3. **Language (R31)**: artifacts are English; German only in old
   engine stderr quotes.
4. **Claims vs. proof (R6/R7/R32)**: any capability statement must
   point at the file/test/gate that proves it; measured numbers say
   on which machine they ran.
5. **Skill files** (`.agents/skills/*/SKILL.md`): frontmatter per the
   2026 Agent Skills spec — `name` lowercase-hyphen and equal to the
   directory name, `description` a quoted ASCII sentence with what+when,
   the frontmatter block pure ASCII (R29/R30). Body < 5000 tokens;
   long material linked, not inlined.
6. **session.md specifics**: the current round section must carry the
   R33 session-start marker (`**Session start (R33):** … Read: …`);
   rounds older than the current one belong in
   `.agents/docs/SESSION-ARCHIVE.md` (append, never delete).

## Step 4 — Fix in place, verify, commit

- Fix each finding in the same pass; keep a per-file list in
  `session.md` (owner rule: work first, docs second, PR last — the
  audit itself is doc work and gets its own section).
- Re-run the mechanical checks; the audit is done when all four are
  green and the per-file checklist is checked.
