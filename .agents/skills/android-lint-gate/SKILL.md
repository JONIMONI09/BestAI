---
name: android-lint-gate
description: "Understand and fix the Android lint job of the Hydra-Stone CI: read its annotation output, know which checks are lint-only (ResourceType, PluralsCandidate, AutoboxingStateCreation, UnusedResources), and apply the correct APIs. Use when the Android lint job is red or when changing strings.xml, res/ resources, or Compose state."
---

# Android Lint Gate — Hydra-Stone

The `Android lint` job of the `Lint & Security` workflow runs
`gradle lintDebug` on the app and publishes its findings as
check-run annotations (the log and artifact hosts are blob-blocked
for API-only clients, so the annotations + the PR-comment attempt are
the machine-readable record). It must be green before merge: it is the
only gate that sees the LINT-ONLY classes (see below).

## When to use

- The `Android lint` job is red (read the annotations FIRST — see
  `.agents/skills/ci-triage`).
- Before changing `android/app/src/main/res/values/strings.xml`,
  any `res/` resource, or Compose state declarations.

## Reading the job output

```bash
gh api repos/JONIMONI09/BestAI/check-runs/<job_id>/annotations --jq \
  '.[] | .annotation_level + " | " + (.path // "n/a") + ":" + (.start_line|tostring) + " | " + (.message[:300])'
```

The gate's own failure annotation is self-describing by design
(2026-10-26 gate fix, R25): if gradle died before the analysis, the
annotation contains the gradle compile tail; if findings exist, each
finding is annotated at its file:line. If you only see a generic
"exit code 1" without that detail, the gate's reporting path itself
broke — treat it as a gate bug, not as an unknown finding.

## The lint-only classes (kotlinc cannot catch these — stub check stays green)

| Check | Trigger | Correct API |
|---|---|---|
| `ResourceType` | `getString(R.plurals.…)` / `getString(R.drawable.…)` | `resources.getQuantityString(id, quantity, vararg)` for plurals; `@DrawableRes` needs `getIdentifier`/`R.drawable` on the right call |
| `PluralsCandidate` | `<string>` with `%d` followed by a word (e.g. "%d tokens") | `<plurals>` with `one`/`other` items; read via `resources.getQuantityString` (pass the count TWICE: quantity + the `%1$d` arg) |
| `AutoboxingStateCreation` | `mutableStateOf(x)` with a primitive `x: Float/Int/…` | `mutableFloatStateOf(x)` / `mutableIntStateOf(x)` |
| `UnusedResources` | res entry never referenced | remove it or use it; regenerate `tools/kotlin_stub_check/stubs/R_gen.kt` from the new strings.xml |

Verified incidents behind each row: 2026-10-26 runs 40339750614 /
40346890465 / 40348442144 (errors.md round-6 entries).

## Rules when touching resources

1. Count-formatted strings are `<plurals>` from day one (pattern: the
   existing `gen_*` plurals in strings.xml).
2. When a string becomes a plural, change the READER in the same
   commit to `resources.getQuantityString(…)` — the grep-able pattern
   in `MainActivity.kt` (lines ~395/536/658/796) is the reference.
3. After ANY strings.xml change: regenerate `stubs/R_gen.kt` (parse
   the XML, emit `object string`/`object plurals` + referenced
   namespaces) and run `bash tools/kotlin_stub_check/kotlin_stub_check.sh`
   → 0 errors.
4. `tools/string_format_check.py` validates all format placeholders
   statically — run it after touching format strings.
5. Suppressions live in `android/app/build.gradle.kts` (`lint {
   disable += … }`); only `GradleDependency` is disabled by policy.
   A new suppression needs a documented reason in the PR body.
