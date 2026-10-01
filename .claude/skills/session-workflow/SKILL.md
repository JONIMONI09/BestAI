---
name: session-workflow
description: "Mandatory session protocol: read session.md, errors.md and rules.md before touching code, keep the plan updated after every step, log every error immediately."
---

# Session Workflow — Hydra-Stone

Mandatory working protocol for every session. **A session is not
complete unless `session.md` and `errors.md` are updated.**

## Files

| File | Purpose | Update trigger |
|---|---|---|
| `session.md` | What is being done, current plan, progress | **At session start, before touching code, and after EVERY completed step** |
| `errors.md` | Every error encountered + its fix | **Immediately, every time an error is hit** |
| `rules.md` | Standing working rules for the agent | When a new rule is agreed or learned |

## Session start (MANDATORY, before touching any code)

1. **Read `session.md`** — what was done last, what is open.
2. **Read `errors.md`** — known pitfalls; check whether the current task
   touches an area with past errors.
3. **Read `rules.md`** — refresh the standing rules.
4. **Write the plan into `session.md`** (Plan section): task, steps,
   acceptance criteria. ALWAYS write it, even for small tasks.

## During work (MANDATORY)

- After **every** completed step: update the plan/status in
  `session.md`. No exceptions — a stale plan is a broken plan.
- On **every** error (build failure, test failure, wrong API, CI red,
  emulator crash, …): add an entry to `errors.md` **immediately** with:
  - Symptom (exact error message or behavior)
  - Root cause (once known)
  - Fix (what actually resolved it)
  - Prevention (how to avoid it next time)
  Never rely on memory — write it down while the context is fresh.

## Session end (MANDATORY)

1. `session.md`: mark done steps ✅, move unfinished ones to "Open",
   summarize what changed.
2. `errors.md`: verify every error hit during the session is recorded.
3. Both files are part of the repo — include them in the commit/PR.

## Format templates

`session.md`:

```markdown
# Session — <date>
## Done (last sessions)
- ...
## Current task
<one sentence>
## Plan
- [ ] step 1
- [ ] step 2
## Status / Notes
<what is running, what is blocked, open questions>
```

`errors.md`:

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
