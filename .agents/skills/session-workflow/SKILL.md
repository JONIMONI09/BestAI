---
name: session-workflow
description: "Mandatory session protocol: read .agents/session.md, .agents/errors.md and .agents/rules.md before touching code, keep the plan updated after every step, log every error immediately."
---

# Session Workflow — Hydra-Stone

Mandatory working protocol for every session. **A session is not
complete unless `.agents/session.md` and `.agents/errors.md` are updated.**

## Files

| File | Purpose | Update trigger |
|---|---|---|
| `.agents/session.md` | What is being done, current plan, progress | **At session start, before touching code, and after EVERY completed step** |
| `.agents/errors.md` | Every error encountered + its fix | **Immediately, every time an error is hit** |
| `.agents/rules.md` | Standing working rules for the agent | When a new rule is agreed or learned |

## Session start (MANDATORY, before touching any code — rules R33)

Read, in this order (only the CURRENT round of session.md; the old
rounds live in `.agents/docs/SESSION-ARCHIVE.md`):

1. **Read `AGENTS.md`** (repo root) — the protocol + the file map.
2. **Read `.agents/session.md`** — the current round: what is open,
   what the owner directed.
3. **Read `.agents/errors.md`** — the top recent entries; check
   whether the current task touches an area with past errors.
4. **Read `.agents/rules.md`** — the standing rules (R1–R33).
5. **Read `.agents/user.md`** — the owner's standing directives
   (highest priority after safety).
6. **Read `.agents/Environment.md`** — the section for this AI only
   (each AI writes only its own section).
7. **Read the task-relevant SKILL.md** files from `.agents/skills/`.

THEN, **before the first code edit**, write the verifiable marker
into the current round section of `.agents/session.md`:

```
**Session start (R33):** <date> (branch ...). Read: <the file list>.
```

CI gate 1j (`tools/session_start_check.sh`) fails the build when the
current round section lacks the marker — a skipped read is a red
check, not a habit (2026 finding: prose-only rules are followed
probabilistically; only enforcement guarantees it).

8. **Write the plan into `.agents/session.md`** (current round
   section): task, steps, acceptance criteria. ALWAYS write it, even
   for small tasks.

## During work (MANDATORY)

- After **every** completed step: update the plan/status in
  `.agents/session.md`. No exceptions — a stale plan is a broken plan.
- On **every** error (build failure, test failure, wrong API, CI red,
  emulator crash, …): add an entry to `.agents/errors.md` **immediately** with:
  - Symptom (exact error message or behavior)
  - Root cause (once known)
  - Fix (what actually resolved it)
  - Prevention (how to avoid it next time)
  Never rely on memory — write it down while the context is fresh.

## Session end (MANDATORY)

1. `.agents/session.md`: mark done steps ✅, move unfinished ones to "Open",
   summarize what changed.
2. `.agents/errors.md`: verify every error hit during the session is recorded.
3. Both files are part of the repo — include them in the commit/PR.

## Format templates

`.agents/session.md` (current structure):

```markdown
# Session Log — Hydra-Stone
> Protocol: see `.agents/skills/session-workflow/SKILL.md`.
## Done (previous sessions)
- ✅ <one line per closed round, newest first>
## Round <YYYY-MM-DD> (<round title>)     <- newest round LAST
**Session start (R33):** <date> (branch ...). Read: <file list>.
<owner directives / context>
**Plan:**
- [ ] step 1
- [ ] step 2
**Progress:**
- [x] step 1 — result
```

Rounds older than the current one are moved verbatim to
`.agents/docs/SESSION-ARCHIVE.md` (append, never delete) so the live
file stays small (R33 context-budget finding).

`.agents/errors.md`:

```markdown
# Error Log
## <YYYY-MM-DD> — <short title>
- **Symptom:** <exact message/behavior>
- **Cause:** <root cause>
- **Fix:** <what resolved it>
- **Prevention:** <how to avoid>
```

## Rationale

These files exist because multi-session work loses context: the plan
that is not written down is the plan that gets re-done wrong, and the
error that is not logged is the error that gets hit twice.
