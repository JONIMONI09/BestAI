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
| 4 | `rules.md` | 32 standing rules (R1–R32). |
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

## Related (not moved, by design)

- `../.claude/skills/` — tool-specific skill definitions (Claude Code
  auto-discovers this path; the skills reference the files above).
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
