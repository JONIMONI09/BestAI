# .agents/ — everything an AI agent must read, in one place

> Moved here 2026-10-26 by owner directive ("in .agents Ordner alle Dateien,
> wie Rules etc auch hinzufügen"): **all** agent-specific documentation now
> lives in this folder. The only agent docs that stay at the repository
> root are `README.md` (project) and `AGENTS.md` (agent entry point).

## Start here, in this order

| # | File | What it is |
|---|------|------------|
| 1 | `../AGENTS.md` | Single entry point: protocol, architecture, gates. |
| 2 | `session.md` | What was done, current plan, progress. **Read before code; update after every step.** |
| 3 | `errors.md` | Every error ever hit + root cause + fix + prevention. |
| 4 | `rules.md` | 33 standing rules (R1–R33; R33 = verifiable session-start read protocol, enforced by gate 1j). |
| 5 | `user.md` | **Owner's standing directives** — highest priority after safety. |
| 6 | `Environment.md` | Per-AI sandbox notes: tested tool install paths, failures, workarounds. |

## Docs (`docs/`)

| File | What it is |
|------|------------|
| `ANDROID-AUDIT.md` | Android toolchain/CI audit trail (evidence-based). |
| `ANDROID_SKILL.md` | Android NDK/JNI build + verify, long form of the skill. |
| `RESEARCH.md` | External research: BitNet b1.58 (W1.58A8), bitnet.cpp I2_S. |
| `HYDRA2-RESEARCH.md` | v2 format research brief (bounded measured memory, load strategies). |
| `gpu-feasibility.md` | GPU acceleration feasibility: measured vs. blocked, split clearly. |
| `pr-gpu-feasibility.md` | The PR companion for the GPU feasibility work. |
| `TECHNICAL-AUDIT.md` | Evidence-based technical audit of the whole repo (2026-10-26). |
| `RESEARCH-LOG.md` | Permanent in-repo research record (owner decision 2026-10-26): crash root causes, CI decisions, investigations with follow-up obligations. **Append, never delete.** |
| `SESSION-ARCHIVE.md` | The complete session.md round log for everything before round 6 (2026-10-26), moved verbatim when session.md was slimmed (R33 context-budget finding). **Append, never delete.** |

## Skills (`skills/` — canonical home, 2026 Agent Skills spec)

| Skill | What it is |
|-------|------------|
| `session-workflow` | The session protocol: start (read order + R33 marker), work, finish. |
| `engine-ci-verify` | The local quality gates in CI-identical order, before any commit touching C/Kotlin/tools. |
| `android-ndk-build` | Android toolchain bootstrap, APK build, no-KVM emulator verification, the 8 known pitfalls. Long form: `docs/ANDROID_SKILL.md`. |
| `ci-triage` | Triage a red CI run: annotations, the zero-job compile-failure signature, local gate replicas. |
| `kotlin-stub-check` | The local kotlinc stub type-check: setup, run, and the stub-fidelity rules. |
| `android-lint-gate` | The Android lint job: reading its annotations, the lint-only classes, the correct APIs. |
| `repo-docs-audit` | The repeatable audit of every .md file in the repo (stale paths, dead refs, banned claims, language). |

`.claude/skills` is a symlink to `skills/` so Claude Code's
auto-discovery keeps working while all agent docs stay in `.agents/`
(R30).

## Related (not moved, by design)

- `docs/` at the root — **product** documentation (FORMAT.md, FORMAT-V2.md,
  GGUF-IMPORT.md, ARCHITECTURE.md): read by humans and agents, shipped with
  the project.
- `tools/kotlin_stub_check/` — local Kotlin type-check without the Android
  SDK (how the round-6 CI compile errors were caught locally).

## Maintaining this folder

- A session ends with `session.md` + `errors.md` updated and committed (R4).
- New owner rules → `user.md` (dated), never only in chat history.
- New environment finding → `Environment.md`, own section only, with
  reason + date + solution (directive 20).
- Research with a follow-up obligation → `docs/RESEARCH-LOG.md`.
