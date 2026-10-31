# `docs/` — the user-facing format + design docs

| Document | The contract it carries |
|---|---|
| `FORMAT.md` | The `.hydra` v1 binary format (versions 1–4), the loader validation rules, the v3/v4 transformer layout, error codes, model-size/RAM |
| `FORMAT-V2.md` | The `.hydra2` container format (implemented, rounds 12–16): element types incl. the resident Q4_0, expert layouts 0/1/2, the routing + inference metadata, the validation rules, the (not-measured) section 6 performance model |
| `GGUF-IMPORT.md` | What GGUF import actually does — v1 projection vs full-fidelity v2, the six accepted tensor layouts, the `--t4`/`--probe` surface, refusal behaviour |
| `ARCHITECTURE.md` | The engine architecture and memory model (zero-heap mmap inference, the v3/v4 passes, the MoE engine's place in it) |
| `LARGE-MODELS.md` | The honest 700B→8B analysis: what fits where, the 2026 technique scan, the phone-path milestone order, the round-16 3B/T4_0 record (§8) |

Rules for these files: English only (R31), no claimed capabilities (R6),
every measurement labelled with machine + date (R32), and
`tools/docs_claim_check.sh` forbids unqualified "works/100%/guaranteed"
claims in any `.md` of the repo. Dated point-in-time audits live in
`.agents/docs/` instead.
