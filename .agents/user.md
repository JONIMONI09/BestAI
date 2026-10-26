# user.md — standing directives of the owner (JONIMONI09)

> Everything in this file is an **explicit, verbatim or near-verbatim owner
> instruction** collected across sessions (kept in English per R31). These
> outrank every convenience and every default agent behaviour. When a task
> conflicts with one of these, the directive wins — ask via a question,
> do not guess.
>
> Maintain: when the owner states a new standing rule, append it here with
> the date. When the owner reverses one, delete it here (note the reversal
> in the changelog, not here).

## Process

1. **(2026-10-25)** "Machen einen Plan!!!!" — always work from an explicit
   plan written into `.agents/session.md` before touching code.
2. **(2026-10-25)** "erst arbeiten, dann Doku, dann PR" — work first,
   documentation second, PR last.
3. **(2026-10-26)** "PR messages = FACTS ONLY" — PR titles, bodies and
   comments contain only what was fixed/changed/verified. No conversation
   references, no connector/Linear/internal-process mentions.
4. **(2026-10-26)** Before ANY code change: read `.agents/`. At the end of
   the round: update `.agents/` (new rules → this file).
5. **(2026-10-26)** "in .agents Ordner alle Dateien, wie Rules etc auch
   hinzufügen" — all agent files (rules, errors, session, environment,
   skills docs, research, audits) live in `.agents/`; only `README.md` and
   `AGENTS.md` stay at the repository root.
6. **(2026-10-26)** "Immer Internet benutzen" — always use the internet
   (web search, docs, package indexes) when facts are needed; never guess
   API behaviour from memory.
7. **(2026-10-26)** Load/install missing binaries and tools (via pip, npm,
   curl, …) instead of skipping a check — and record the tested path in
   `.agents/Environment.md`.
8. **(2026-10-25)** Check Linear (and Notion) when the user mentions
   tasks/decisions from outside the repo.
9. **(2026-10-26)** Ask via the `ask_user` tool when a question arises;
   never block silently.

## Quality / correctness

10. **(2026-10-25)** "Zero stubs, zero fake checkboxes" — a stub, a
    hardcoded return, or a checked box for behaviour that is not implemented
    is a defect, not a deliverable.
11. **(2026-10-25)** Baseline (v1 engine + tests) is untouchable: additive
    changes only; never delete a working path to hide a problem.
12. **(2026-10-25)** "NEVER defer local C++/lint checks to CI" — run them
    locally, now, and show the real output. "CI will catch it" is not an
    acceptable reason to skip a local check.
13. **(2026-10-25)** Every crash → regression test; every CI failure →
    regression test + an `.agents/errors.md` entry.
14. **(2026-10-26)** PR CI must verify functionality **and**
    release-readiness without performing a release; CI must catch
    portability/error classes before merge.
15. **(2026-10-26)** "I REJECT your lazy conclusion" (re: claiming a
    constraint is "satisfied by construction" without engineering it) —
    no hiding behind documentation caveats; do the real work and prove it
    by execution.
16. **(2026-10-25)** Proactive bug hunting is expected: do not stop at the
    reported symptom.
17. **(2026-10-25)** CI speed-up is welcome, but only if it stays safe
    (no lost coverage).

## Artifacts / communication

18. All artifacts (code comments, docs, CI output, PR text) in **English**;
    talk to the user in **German** (R31 + owner preference).
19. **(2026-10-25)** Memory goes into the repo: session/error/environment
    records are committed, not kept in agent-local scratch space.
20. **(2026-10-25)** `Environment.md` discipline: each AI agent updates
    ONLY its own section; every recorded failure needs reason + date +
    solution.
21. **(2026-10-25)** Document everything: "Document everything!!! Read all
    the rules etc!" — new gates, new formats, new decisions get a doc
    pointer in the same change.
