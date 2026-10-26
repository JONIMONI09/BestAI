---
name: kotlin-stub-check
description: "Type-check all app Kotlin of the Hydra-Stone Android project locally without the Android SDK, using the kotlinc stub classpath in tools/kotlin_stub_check. Use before committing any .kt change, when CI's Android lint/APK jobs fail at Kotlin compile, or when adding a new Android/compose API to the app."
---

# Kotlin Stub Check — Hydra-Stone

The sandbox has no Android SDK (Maven blocked), but the Kotlin
COMPILER is on npm. The stub classpath in
`tools/kotlin_stub_check/stubs/` declares the exact Android /
androidx / Compose / org.json API surface the app uses, so `kotlinc`
can type-check all 10 app `.kt` files in ~20 s.

## When to use

- Before committing ANY `.kt` or `res/values/*.xml` change (CI parity
  for the Kotlin compile step of the Android lint + APK jobs).
- When CI fails with `Unresolved reference …` in Kotlin: reproduce
  here first (the `.agents/skills/ci-triage` flow).
- When the app starts using a new Android/compose API.

## Setup (per turn — /tmp is ephemeral)

```bash
mkdir -p /tmp/ktnpm && cd /tmp/ktnpm
npm install kotlin-compiler@2.4.21 javajre-linux-64@17.0.8
```

Verified: kotlinc-jvm 2.4.21 on JRE 17.0.8 (matches the CI Temurin 17
major). ~1 minute.

## Run

```bash
bash tools/kotlin_stub_check/kotlin_stub_check.sh
# expected: "kotlin stub check: OK (0 errors)"
```

Exit 0 + "OK (0 errors)" is the ONLY acceptable result. Any `error:`
line means the app OR the stub is wrong (see fidelity rules).

## Maintaining the stubs (the rules that were learned the hard way)

1. **One package per stub file** (Kotlin language rule).
2. **Verify every signature against the real API** —
   developer.android.com / composables.com / the framework source —
   NEVER guess. Fabricated stub members pass the check and fail CI
   (2026-10-26: invented `Context.getQuantityString`, the real method
   is on `Resources`; see errors.md).
3. **Scope faithfulness matters**: Compose `Modifier.weight` is a
   RowScope/ColumnScope MEMBER extension — stub it as a scope member,
   NOT as a top-level function with an import (K2 rejects the import
   form with "Cannot access 'val RowColumnParentData?.weight'").
   Compose `items(key = …)` is single-parameter `(item: T) -> Any`
   (the two-parameter one is `itemsIndexed`).
4. **R is generated from the real strings.xml**: parse
   `android/app/src/main/res/values/strings.xml`, emit
   `object R { object string { const val … } object plurals { … } }`
   plus every namespace referenced from the `.kt` files. Regenerate
   whenever strings.xml changes.
5. **Stub errors mean app OR stub is wrong** — the real API source is
   the tie-breaker. The `weight` episode proved this in both
   directions in one round.

## Known limitations (what this check CANNOT catch)

kotlinc checks types, not Android resource annotations or lint rules.
CI's Android lint job is the authority for these — it must stay green
before merge:

- **ResourceType** — `getString(R.plurals.…)`, `getString(R.drawable.
  …)` etc. compile here but fail lint. Plurals are read with
  `resources.getQuantityString(id, quantity, vararg args)`.
- **AutoboxingStateCreation / PluralsCandidate / UnusedResources** and
  other Compose/resource lint checks.
- **Compose compiler transforms** (the stub carries only the
  `@Composable` annotation; the transform itself does not run).

A green stub check means "the API surface is spelled correctly", not
"the app is lint-clean".
