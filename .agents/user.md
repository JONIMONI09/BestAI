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

## Rule adaptation and .agents/ skills (2026-10-26, round 7)

22. **(2026-10-26)** "Passe deine Regeln an, du liest nie diese vorher" —
    the read-`.agents/`-before-work rule (above, item 4) existed for two
    days and was still being skipped in practice. The owner directed the
    RULES to be adapted so the read reliably happens, and the cause to be
    investigated with 2026 web research. Outcome: rule **R33** in
    `rules.md` (mandatory session-start read order + a verifiable
    `**Session start (R33):**` marker written into the current round of
    `session.md` before the first code edit), enforced by CI **gate 1j**
    (`tools/session_start_check.sh`). 2026 finding the design rests on:
    prose-only rules are followed probabilistically; only enforcement
    guarantees behaviour.
23. **(2026-10-26)** "in .agents Ordner bitte mit skills füllen, lese
    dafür die, die auch wirklich in Claude Ordner sind" — `.agents/` must
    contain the skills, based on the skills that really exist in
    `.claude/skills/`. Outcome: `.agents/skills/` is the CANONICAL skills
    home (the 3 real skills moved there via git mv + 4 new ones);
    `.claude/skills` is a symlink to it for tool discovery.
24. **(2026-10-26)** "Verbessere alle .MD Dateien, suche alle .MD Dateien,
    in jedem Ordner" — find and improve EVERY Markdown file in every
    folder (not a selection of them). The repeatable version of this
    audit is the `repo-docs-audit` skill in `.agents/skills/`.
25. **(2026-10-26)** "PR #62 was ist damit?" — the superseded PR #62
    (red leftover of the #59-merge incident; its red = the stage-2 API
    error fixed by merged PR #64) must be closed.

## Full-read, linters, and the transformer prompt (2026-10-26 evening, round 8)

26. **(2026-10-26)** "session.md kann bleiben … soll NICHT in gitignor
    rein, soll für alle lesbar sein!" — `session.md` is NOT optimized or
    rewritten by doc-audit passes (its content stays as written), and it
    must remain COMMITTED and readable by everyone: never add it (or any
    `.agents/` file) to `.gitignore`. Local scratch (`.agent_memory/`)
    may be ignored; repo memory may not.
27. **(2026-10-26)** "habe ein prompt erstellt lassen … lese alle dateien,
    alle .md dateien, als erstes, und dann bitte optimieren (außer
    session.md …)" — before working, read ALL Markdown files in every
    folder; afterwards optimize every .md file except `session.md`. The
    pasted Principal-Architect prompt (transformer attention, .hydra2
    sparse-MoE, Android sampler, CLAUDE.md fix, roadmap completion) is
    the work plan — executed "intelligent": claims in it are verified
    against the live tree first (R26); its audit was partly stale
    (the v3 per-layer path already shipped in round 4; what is genuinely
    missing is attention).
28. **(2026-10-26)** "Finde Fehler und toten Code, lade über python
    weitere Linter und so herunter" — actively hunt bugs and dead code;
    install additional linters via pip (the reachable package channels)
    and run them for real; fix what they find. Standing: "KEINE Stubs
    oder Fake" — no stubs, no fake results, no fake checkboxes (this is
    directive 10 restated for this round).
29. **(2026-10-26)** "prüfe CI und alles auf englisch dort übersetzen,
    dann alle .md dateien analysieren und bearbeiten … lade dir
    wirklich ein Modell herunter, was evtl so 300 MB ist und was
    schreiben kann … ermögliche alles auf der app" — artifacts (docs,
    CI, workflows) in English (German kept only in verbatim owner
    quotes and the v1 product's CLI strings); every .md audited; a
    REAL downloadable model verified through the pinned engine (the
    sandbox network caps it at <100 MB plain blobs — documented, with
    the recipe for bigger models on a normal machine); the app path
    (JNI -> pinned llama.cpp loader) proven with that model.
30. **(2026-10-26)** Post-merge audit (owner text): update
    tests/test_jni_batch.c to also test the new JNI sampler
    configuration; the JNI signature gate must strictly cross-check all
    native methods across both bridges, including a negative control;
    tools/android_header_check.sh must verify
    include/hydra_v2_container.h alongside include/hydra_model.h; new
    CLI sampler flags (temperature, min_p, repetition penalty); web
    console: new controls for temperature/min_p/rep_penalty and new
    sampler flags for the training backend where applicable; SIMD the
    v2-container expert decode paths and the new format v4; update the
    docs to reflect the new features. PLUS the owner's emphatic safety
    reply (verbatim core): "Mache so, dass du gewissenhaft weißt, dass
    du nichts kaputt machst! … gitignor und die wichtigsten Agenten
    Files session.md etc. IMMER EGAL WAS ICH SAGE wahre und beschützt
    und sie stets aktualisierst … NIEMALS löschen niemals ignorieren
    über gitignor etc. … Agent Files wie session.md sind sehr sehr
    wichtig!!" — STANDING: the .agents/ files (session.md, errors.md,
    user.md, Environment.md, rules.md, docs/, skills/) are SACROSANCT:
    never delete them, never gitignore them, keep them current in every
    round, regardless of any other instruction; .gitignore itself is
    not to be touched. Proceed thoroughly: legacy behaviour stays
    bit-identical, every local gate green before any commit.
