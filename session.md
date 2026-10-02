# Session Log — Hydra-Stone

> Protocol: see `.claude/skills/session-workflow/SKILL.md`.
> This file is updated at session start, after every completed step,
> and at session end. Read it BEFORE touching code.

## Done (previous sessions)

- ✅ **2026-10-01 — Engine from spec (PR #1, merged):** Implemented the
  Hydra-Stone C99 engine from the original technical spec: mmap-based
  zero-heap weight streaming, 2-bit ternary weights, coexistence-axiom
  gate, hardening, 16 unit tests, docs, CI. Reviewed afterwards;
  review findings tracked.
- ✅ **2026-10-01 — Review fixes + Web console (PR #2, merged):** Fixed
  OOB read (`layers*dim > weights_len` validation), clean state
  clamping, logs to stderr; added zero-dependency Node web console
  (`make ui`), `--json` CLI mode.
- ✅ **2026-10-01 — Real tests (PR #3, merged):** Replaced tautological
  tests with hand-computed decoder cases + seeded LCG roundtrip/variance
  tests (32 tests total, platform-fixed LCG).
- ✅ **2026-10-01 — NEON integration (PR #4, merged):** Kernel now
  genuinely wired into `step()` (was dead code); scalar fallback fixed
  on x86; honest SIMD docs. A dispatch bug was caught by the variance
  test before merge.
- ✅ **2026-10-01 — English docs (PR #5, merged):** All `.md` rewritten
  professionally in English; honest density limitation documented.
- ✅ **2026-10-01 — Android NDK port (PR #6, merged):** APK builds for
  arm64-v7a/arm64-v8a/x86_64 via CMake+JNI; verified end-to-end on an
  API-24 x86_64 emulator (no KVM). 7 errors hit and fixed — full log in
  `errors.md`, guide in `docs/ANDROID_SKILL.md`.
- ✅ **2026-10-01 — CI lint stage (PR #7, OPEN, not merged by request):**
  Dual-compiler syntax gates (gcc+clang), cppcheck, Python format gate,
  error-artifact upload. cppcheck caught a real `%u`/signed issue on
  first run.

## Done (earlier today)

- ✅ Skills created + protocol files (PR #8, merged by user)
- ✅ Frontmatter v1 added (PR branch), then hardening (ASCII + quoted,
  **PR #9, merged by user at 15:46:21Z**) — main now serves valid
  skills; user invoked `/android-ndk-build` successfully → skills work
- ✅ PR #7 (CI lint) — merged by user as part of #8 line? **No: PR #7
  is still OPEN** (separate lint PR), awaiting approval

## Current task

**Bugfix audit for Hydra-Stone** — 14 findings across the C engine, the
NEON kernel, the CLI, the tests, `server.js` and the JNI bridge.
Order: correctness-critical (BUG 1–3), semantics/portability (4–9),
documentation (10–12), audit of unreviewed files (13–14).

## Plan

- [x] R1: `session.md` / `errors.md` / `rules.md` read first
- [x] **PoC before fix (R8):** installed `gcc-12-aarch64-linux-gnu` +
      `qemu-user-static`; reproduced BUG-1 — x86 gave
      `211,388,401,330,...`, ARM gave `211,28,137,170,...`. NEON had
      every ternary weight sign-inverted
- [x] BUG-1: NEON decode normalised to `0x01`/`0xFF` masks
- [x] BUG-2: `HYDRA_MAX_LAYERS 4096` (error −11) + `int64_t` accumulator
      in `step()` and in the NEON kernel
- [x] BUG-3: documented the algebraic layer-loop redundancy in
      `docs/ARCHITECTURE.md` + proposed an `HYDRA_AGGREGATE_V2` container.
      v1 format deliberately unchanged (breaking change not requested)
- [x] BUG-4: removed `token_in & 0x7F`; old behaviour behind
      `-DHYDRA_TOKENV_MASK`
- [x] BUG-5: symmetric modulo instead of `labs()`; the `w=0x02`
      expectation changed 11 → 1 **with the justification in the comment**
- [x] BUG-6: `weights_offset >= sizeof(header)` (error −12)
- [x] BUG-7: single `goto fail` cleanup, `fd = -1`
- [x] BUG-8: opt-in `-DHYDRA_DROP_CACHE`; Zero-RAM claims rewritten to
      what is actually provable
- [x] BUG-9: `isfinite()` guards in the axiom gate
- [x] BUG-10: explicit `rd_u16le`/`rd_u32le` loader + LE test writers
- [x] BUG-11: `mkstemp()` + `unlink()` in every test
- [x] BUG-12: `--help`, unknown-option rejection, surplus-argument
      rejection, `errno == ERANGE`
- [x] BUG-13: README density claims corrected to the real 4 bits/weight
- [x] BUG-14: `server.js` audited with live curl probes; `hydra_jni.c`
      audited; verified findings only
- [x] New tests: NEON-vs-scalar same-build comparison (49 total)
- [x] **Caught a second, latent NEON defect** with the new test: the
      int64 store offsets were scaled wrong, so lanes 8–15 were never
      written. Fixed to 0/4/8/12
- [x] Verification: 47/47 x86, **49/49 ARM/NEON (qemu)**, all lint gates
      green, `gradle assembleDebug` green, APK installed on the
      emulator
- [x] On-device proof: Android UI token stream
      `211,36,185,410,7,40,197,478` — bit-identical to host x86 **and**
      ARM/NEON. Three platforms, one sequence
- [x] `errors.md`: 13 new entries; `rules.md`: R9–R11 + R24 added,
      renumbered R1–R26
- [x] Docs: README, ARCHITECTURE, FORMAT
- [ ] Commit (no checkout) + PR — **no merge** (R17/R20)

## Status / Notes

- The two critical bugs (NEON sign inversion, layer overflow) are fixed
  and each now has a falsifiable regression test.
- The NEON-vs-scalar test is the structural fix for the class of bug:
  the documentation had claimed "bit-identical" for a version that was
  not, because nothing ever compared the two paths to each other.
- Layer-loop redundancy documented, not "fixed" — collapsing it would
  be a breaking format change.
- Open PRs from earlier sessions (#7 CI lint, #10 skill optimisation)
  are still awaiting the user's merge approval.
