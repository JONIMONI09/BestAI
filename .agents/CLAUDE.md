@AGENTS.md

## Session protocol (binding for every agent in this repository)

This file bridges Claude Code to the repository's agent system. The
single source of truth for agent work is `AGENTS.md` (loaded above);
this file adds nothing of its own and may not diverge from it. The
repository root carries a REAL `CLAUDE.md` bridge file (only
`@AGENTS.md` + `@.agents/CLAUDE.md` imports, no rules of its own) so
Claude Code discovers the instructions on every OS and version —
including Windows checkouts where symlinks are disabled.

Rules that always apply, in one line each (details: `rules.md`):

- R1/R2 — read `session.md` first, keep its plan section current.
- R5/R6/R7/R32 — no fake tests, no unproven claims, measure first.
- R17 — never merge without the user's explicit approval.
- R20 — never run `git checkout`.
- R31 — all artefacts (commits, PRs, code, docs) in English.
- R21 — run the full local verification gate before committing.
- M6 invariant — a README roadmap checkbox may be marked done ONLY for
  work that is 100% implemented AND verified by an automated test in
  `tests/` or `tools/`. No partial credit.
