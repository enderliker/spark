# Implementation Plan: Spark language roadmap (areas 1-7)

## Overview

Spark today is a working bytecode interpreter (95 tests, fib(27) ~9.5 ms). The goal
of this plan is to fill the seven gaps identified for the language: syntax, data
types, functions, standard library / I/O, VM performance and limits, semantics, and
tooling/ecosystem. This is a **multi-phase, multi-session roadmap**; it is planned in
full here but is meant to be executed phase by phase with checkpoints, keeping the
build warning-free, the test suite green and the benchmark within budget after every
task.

The plan is ordered by **value/risk ratio** and by dependencies, not strictly by the
original numbering:

1. High value, low risk, no core changes (collections & string library).
2. High value, low risk syntax (const, ternary, `??`, switch, labels).
3. Foundational but high risk, must come before bitwise operators (64-bit integers).
4. Makes the language usable for real scripts (I/O, argv, exit).
5. Function/semantics refinements (TCO, default args, varargs).
6. Big features (modules, exceptions, objects/classes).
7. VM performance & limits (raise caps, growable stack, GC, bytecode cache).
8. Ecosystem/tooling (debugger, formatter, linter, CI, concurrency).

## Definition of Done (every task)

- Build is warning-free: `make` with `-O3 -Wall -Wextra -Wpedantic`.
- `python3 tests/run_tests.py` passes (new behavior has new fixtures).
- Sanitizer run is clean when the task touches the VM/GC/compiler:
  `make clean && make CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic" LDFLAGS="-fsanitize=address,undefined" && python3 tests/run_tests.py && make clean && make`.
- `python3 benchmarks/run.py` minimum stays **<= 15 ms** unless the task is explicitly
  about performance (then state the target).
- Documentation updated: `README.md`, `docs/language.md`, `docs/architecture.md`.
- All artifacts in English (`AGENTS.md`).

## Architecture Decisions

> Decisions marked **[decided]** were confirmed by the user on 2026-10-23.

- **A1 - Integer representation. [decided]** Add `VAL_INT` with `int64_t` to the
  existing tag+union `Value` (keep `ValueType`/`Obj` as-is; do **not** do NaN-boxing).
  Mixed `int`/`number` arithmetic promotes to `double`; integer overflow also promotes
  to `double`. This keeps the GC and value layout simple and `-Wpedantic` clean.
- **A2 - `type()` compatibility. [decided]** `type()` keeps returning `"number"` for
  both ints and floats to avoid breaking existing fixtures/docs; add `int()`, `float()`
  and `is_int()` instead. (Was Q2.)
- **A3 - Division. [decided]** `/` stays floating-point division (existing semantics).
  Add `//` (integer division) and keep `%` for the remainder. `/` will **not** become
  integer division when both operands are ints. (Was Q1.)
- **A4 - Native callbacks.** Higher-order natives (`map`/`filter`/`reduce`/`sort_by`)
  need to call a Spark function from C. Implement a re-entrant call helper in `vm.c`
  that pushes a frame and runs a nested interpreter loop, guarded so it does not
  corrupt the current frame (design spike first, T7).
- **A5 - Exceptions.** Implement `try`/`catch`/`finally`/`throw` with an exception
  handler stack in the VM (push handler on `try`, `OP_THROW` unwinds to the nearest
  handler). Uncaught throw becomes a normal runtime error.
- **A6 - Modules.** `import "path"` compiles each module once into an `ObjClosure`,
  caches it by resolved path, and executes it against the **shared global namespace**
  (simplest, REPL-friendly). (Open question Q3.)
- **A7 - Objects. [decided]** Start with method-call sugar on maps
  (`recv.method(args)` reads the key and calls it with the receiver bound to `self`).
  Full classes are deferred until that sugar is in and evaluated. (Was Q5; see T44.)
- **A8 - Concurrency.** Optional/last: coroutines as VM-managed green threads
  (`spawn`/`yield`/channels), not OS threads. (Open question Q6.)

## Task List (ordered index)

### Phase 1 - Collections & string library (area 2)
- [x] T1 Array search natives (`contains`, `index_of`)
- [x] T2 Array mutation natives (`insert`, `remove`, `reverse`, `fill`)
- [x] T3 Array `slice`
- [x] T4 String search natives (`contains`, `index_of`, `starts_with`, `ends_with`)
- [x] T5 String transform natives (`split`, `join`, `trim`, `upper`, `lower`, `repeat`, `replace`)
- [x] T6 Map helpers (`clear`, `merge`, `get` with default)
- [x] T7 Native callback support (re-entrant call) - design + implementation
- [x] T8 Higher-order natives (`map`, `filter`, `reduce`, `each`)
- [x] T9 `sort` / `sort_by`
- [x] T10 Docs + tests for the new natives

### Checkpoint: Phase 1
- [x] All tests pass, build clean, docs updated
- [x] Review with the user before Phase 2

### Phase 2 - Language syntax (area 1, no integers needed)
- [x] T11 `const` declarations
- [x] T12 Ternary operator `?:`
- [x] T13 Null-coalescing `??`
- [x] T14 `do { } while`
- [x] T15 Labelled loops + labelled `break`/`continue`
- [x] T16 `switch`/`case`/`default`
- [x] T17 String interpolation
- [x] T18 Numeric literal sugar (hex/bin/oct, `_` separators)

### Checkpoint: Phase 2
- [x] All tests pass, build clean, docs updated
- [ ] Review with the user before Phase 3

### Phase 3 - 64-bit integers (area 2 core)
- [ ] T19 `VAL_INT`: value helpers, formatting, equality
- [ ] T20 Integer literals in lexer/parser
- [ ] T21 Arithmetic & comparison with int/float promotion
- [ ] T22 Integer division/modulo (`//`, `%`) - per decision A3/Q1
- [ ] T23 Integer map hashing + integer array indexes
- [ ] T24 Integer-aware natives (`int`, `float`, `is_int`, `abs`/`min`/`max`/rounding)
- [ ] T25 Bitwise operators (`& | ^ ~ << >>`, plus `&= |= ^= <<= >>=`)
- [ ] T26 Docs + tests for integers

### Checkpoint: Phase 3 (critical)
- [ ] All tests pass, sanitizers clean
- [ ] Benchmark minimum still <= 15 ms
- [ ] Review with the user before Phase 4

### Phase 4 - Standard library / I/O (area 4)
- [ ] T27 `exit(code)`, `argv`, `env(name?)`
- [ ] T28 stdin (`input(prompt?)`)
- [ ] T29 One-shot file natives (`read_file`, `write_file`, `append_file`, `file_exists`)
- [ ] T30 File object (`open`, `read_line`, `write`, `close`)
- [ ] T31 Random (`rand`, `randint`, `srand`)
- [ ] T32 JSON encode/decode
- [ ] T33 Regex (stretch)

### Checkpoint: Phase 4
- [ ] All tests pass; a real script (read file, parse, write output) works end-to-end
- [ ] Review with the user before Phase 5

### Phase 5 - Functions & semantics (areas 3, 6)
- [ ] T34 Tail-call optimization
- [ ] T35 Default parameters
- [ ] T36 Varargs + spread (`...`)
- [ ] T37 Named arguments (optional)
- [ ] T38 REPL `_` last value
- [ ] T39 Numeric/semantics polish (`is_nan`, NaN equality documented)

### Checkpoint: Phase 5
- [ ] All tests pass, build clean, docs updated
- [ ] Review with the user before Phase 6

### Phase 6 - Modules, exceptions, objects
- [ ] T40 `import` design + implementation
- [ ] T41 Module cache + cyclic-import handling
- [ ] T42 `throw` + `try`/`catch`/`finally`
- [ ] T43 Method-call sugar on maps + `self`
- [ ] T44 Classes (fields, methods, `new`) - optional/advanced

### Checkpoint: Phase 6
- [ ] All tests pass, sanitizers clean; multi-file program works
- [ ] Review with the user before Phase 7

### Phase 7 - VM performance & limits (area 5)
- [ ] T45 Raise limits (frames/locals/constants/jumps; 32-bit operands where needed)
- [ ] T46 Growable value stack
- [ ] T47 GC: incremental or generational
- [ ] T47b Stable merge sort for `sort` / `sort_by` (follow-up from T9)
- [ ] T48 Compiler constant folding + dead-code elimination
- [ ] T49 Precompiled bytecode (`.sparkc`) + `--compile`
- [ ] T50 Native code generation / JIT (stretch)

### Checkpoint: Phase 7
- [ ] Benchmark improved, tests + sanitizers clean
- [ ] Review with the user before Phase 8

### Phase 8 - Tools & ecosystem (area 7)
- [ ] T51 Debugger (`--debug`: breakpoints, step, locals)
- [ ] T52 Formatter
- [ ] T53 Linter
- [ ] T54 CI + coverage + sanitizer job
- [ ] T55 Concurrency (`spawn`/`yield`/channels) - stretch

### Checkpoint: Complete
- [ ] All acceptance criteria met
- [ ] Full sanitizer + benchmark run
- [ ] Ready for review

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Integers destabilize the `double`-based VM, tests and benchmark | High | Dedicated phase with its own checkpoint; int/float promotion rules fixed in A1/A3; benchmark gate <= 15 ms |
| Re-entrant native callbacks corrupt the VM stack/frames | High | T7 is a design spike + isolated helper; sanitizer run; frame guards |
| Exceptions change error propagation in `run()` | High | Keep `had_error` for normal errors, add a separate handler stack; fall back to runtime error when no handler |
| Modules touch parser/compiler/vm and path resolution | Medium | Design task T40 first, shared-global model (A6) |
| GC changes introduce leaks/crashes | High | Sanitizer gate on every GC task |
| Scope creep (55 tasks) | High | Execute phase by phase; user review at each checkpoint; can stop or re-prioritize |
| JIT/native codegen is open-ended | Medium | Mark T50/T55 as stretch; do not block earlier phases |

## Status

**Phase 1 (collections & string library, T1-T10) is implemented.** The new natives
live in `src/vm.c`, are covered by `tests/cases/{collections,strings,higher_order,sorting}.spark`
and `tests/errors/{contains_tipo,map_callback,sort_mixto,slice_negativo,reduce_vacio}.spark`,
and the reference is in `README.md`, `docs/language.md` and `docs/architecture.md`.
Q1, Q2 and Q5 are decided (see A2/A3/A7); the remaining questions below must be answered
before their phase starts.

## Open Questions (need human input)

Resolved:
- **Q1 (division) - DECIDED:** `/` stays float; add `//` for integer division (A3).
- **Q2 (`type`) - DECIDED:** keep `type(x) == "number"`; add `int()`/`float()`/`is_int()` (A2).
- **Q5 (objects) - DECIDED:** method sugar on maps first; full classes deferred (A7).
- **Q8 (priority) - DEFERRED:** plan only for now; the user will give the go-ahead
  before implementation starts.
- **Q9 (tracker) - CONFIRMED:** tasks live in `tasks/todo.md`.

Still open (answer at the start of the relevant phase):
- **Q3 (modules, Phase 6):** shared global namespace vs per-module namespace? Path
  resolution, and module search paths?
- **Q4 (exceptions, Phase 6):** syntax `try { } catch (e) { } finally { }`, uncaught
  behavior, and the type of the caught value. Runtime errors already carry an
  `ErrorType` category; decide whether `catch` receives the category, the message or
  both.
- **Q6 (concurrency, Phase 8):** coroutines (VM green threads), and is it wanted at all?
- **Q7 (breaking changes):** acceptable to change existing test expectations/docs where
  a new feature requires it?
- **Q10 (JIT):** confirm T50 and T55 may be deferred as stretch goals.

## Baseline (for regression checks)

- Tests: 95 passing (`python3 tests/run_tests.py`).
- Benchmark: `fib(27)` minimum ~9.5 ms, median ~10 ms (`python3 benchmarks/run.py`).
- No warnings, sanitizers clean.