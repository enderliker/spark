# Intent: complete Spark

Confirmed with the user on 2026-10-27. This document records the agreed scope,
decisions and work order for finishing the language. It is the input for the
implementation phases; update it if the intent changes.

## Outcome

Spark is closed and coherent: the uncommitted work-in-progress is rescued, every
finding from the audit is fixed, semantics are unified, the VM is optimized
(measured against a baseline), Phase 13 (real OOP) and Phase 14 (coroutines with
channels) are implemented.

## Why now

A previous session left ~2,000 uncommitted lines implementing most of phases
9-12, the parser does not compile, docs/plan are stale and the new work has no
test fixtures.

## Success criteria

- Warning-free build, full suite green, sanitizers clean for VM/GC changes.
- `docs/language.md`, `docs/architecture.md`, `README.md`, `tasks/plan.md`,
  `tasks/todo.md` and `AGENTS.md` match the code.
- `CHANGELOG.md` exists with a "Breaking changes" section.
- Benchmark minimum stays at or below the baseline (currently ~5.1 ms).
- Every new feature has fixtures; fixture commits are separate from VM commits
  so the history stays bisectable.

## Decisions

- **Audit scope:** every finding, no matter how small, is fixed or optimized.
  This includes the large roadmap phases that were never started.
- **Q7 (fixtures/docs):** they may change; the language is pre-1.0.
- **`+` with strings:** stays strict. No implicit coercion. Improve the
  `TypeError` message to suggest interpolation or `to_string()`.
- **Negative indexing:** unified everywhere. Negative values count from the
  end for simple indexing, `slice` and `substr`. A simple index out of range is
  an error; slices clamp. Document the difference.
- **`?.`:** guards method calls as well. `a?.b()` short-circuits when `a` is
  nil. Distinct errors for "`b` is nil" and "`b` is not callable".
- **`type()` for instances:** returns the class name. Class names that collide
  with built-in type names (`number`, `string`, `map`, `list`, ...) are
  rejected. Add `is_a(x, Class)` that respects inheritance.
- **`new` is mandatory:** `new Foo()` constructs; calling `Foo()` without
  `new` is a clear error. One construction form only.
- **Coroutines:** `spawn`/`yield` plus channels; buffered/unbuffered semantics
  proposed before implementation.

## Work order

1. **Rescue the WIP and restore a green baseline.** Fix parser/arena, finish
   half-done work, add fixtures for everything new, commit cleanly. Also close
   the remaining audit findings: stale docs, formatter/linter unaware of the new
   syntax, Spanish artifacts, the two-variable `for-in` over maps bug,
   `++`/`--` on index/field targets, astral-plane `\u` escapes, `substr`, error
   messages.
2. **Unifications (decisions above)** with fixtures.
3. **VM optimization**, measured against the baseline: fused conditional jumps
   for `==`, `!=`, `<=`, `>=`; T57 amortized string concatenation; `OP_INVOKE`
   caching; constant deduplication; line/column delta encoding; and the rest of
   the audit list.
4. **Phase 13 (OOP):** `ObjClass`/`ObjInstance`, single inheritance with
   `extends`/`super`, declared fields, `static`, `to_string()`, operator
   overloading, `type()` returning the class name, `is_a`, mandatory `new`.
   Micro-decisions (super chain, operator list, field visibility) proposed
   before coding.
5. **Phase 14:** `spawn`/`yield` + channels (buffered/unbuffered, blocking
   semantics), design proposed before coding.

## Out of scope

- Implicit coercion in `+` (to be revisited later if needed).
- JIT/AOT native code generation (T50 stays deferred).

## Status

Fulfilled. The WIP was rescued and committed, every audit finding is fixed, the VM
optimizations landed (fused comparison jumps, `OP_INVOKE` caching, constant
deduplication, compressed line metadata), Phase 13 and Phase 14 are implemented, and
`docs/language.md`, `docs/architecture.md`, `README.md`, `tasks/plan.md`,
`tasks/todo.md`, `AGENTS.md` and `CHANGELOG.md` match the code.

One work-order item is resolved differently on purpose: **T57** asked for amortized
`s += x`. Strings stay immutable, so the amortized guarantee comes from the
`string_builder` API instead (documented in `docs/language.md` and
`docs/architecture.md`, guarded by `tests/cases/string_builder_scale.spark`), while
`s += x` keeps its O(N^2) cost and is documented as such.
