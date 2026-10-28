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
- `python3 benchmarks/run.py` minimum stays **<= 5 ms** unless the task is explicitly
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
  Add integer division and keep `%` for the remainder. `/` will **not** become
  integer division when both operands are ints. The planned `//` spelling was
  impossible: `//` is already the line-comment token, so the operator is spelled
  `div` (`a div b`); confirmed with the user on 2026-10-24. (Was Q1.)
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
- **A8 - Concurrency. [decided]** Coroutines as VM-managed green threads
  (`spawn`/`yield`/channels), not OS threads; implemented in Phase 14. Each coroutine
  owns a private value stack and the VM snapshots frames/try-handlers when it suspends.

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
- [x] Review with the user before Phase 3

### Phase 3 - 64-bit integers (area 2 core)
- [x] T19 `VAL_INT`: value helpers, formatting, equality
- [x] T20 Integer literals in lexer/parser
- [x] T21 Arithmetic & comparison with int/float promotion
- [x] T22 Integer division/modulo (`div`, `%`) - per decision A3/Q1
- [x] T23 Integer map hashing + integer array indexes
- [x] T24 Integer-aware natives (`int`, `float`, `is_int`, `abs`/`min`/`max`/rounding)
- [x] T25 Bitwise operators (`& | ^ ~ << >>`, plus `&= |= ^= <<= >>=`)
- [x] T26 Docs + tests for integers

### Checkpoint: Phase 3 (critical)
- [x] All tests pass, sanitizers clean
- [x] Benchmark minimum still <= 5 ms
- [x] Review with the user before Phase 4

### Phase 4 - Standard library / I/O (area 4)
- [x] T27 `exit(code)`, `argv`, `env(name?)`
- [x] T28 stdin (`input(prompt?)`)
- [x] T29 One-shot file natives (`read_file`, `write_file`, `append_file`, `file_exists`)
- [x] T30 File object (`open`, `read_line`, `write`, `close`)
- [x] T31 Random (`rand`, `randint`, `srand`)
- [x] T32 JSON encode/decode
- [x] T33 Regex (stretch)

### Checkpoint: Phase 4
- [x] All tests pass; a real script (read file, parse, write output) works end-to-end
- [x] Review with the user before Phase 5

### Phase 5 - Functions & semantics (areas 3, 6)
- [x] T34 Tail-call optimization
- [x] T35 Default parameters
- [x] T36 Varargs + spread (`...`)
- [x] T37 Named arguments (deferred in favor of map option arguments)
- [x] T38 REPL `_` last value
- [x] T39 Numeric/semantics polish (`is_nan`, NaN equality documented)

### Checkpoint: Phase 5
- [x] All tests pass, build clean, docs updated
- [x] Review with the user before Phase 6

### Phase 6 - Modules, exceptions, objects
- [x] T40 `import` design + implementation
- [x] T41 Module cache + cyclic-import handling
- [x] T42 `throw` + `try`/`catch`/`finally`
- [x] T43 Method-call sugar on maps + `self`
- [x] T44 Classes (fields, methods, `new`) - optional/advanced

### Checkpoint: Phase 6
- [x] All tests pass, sanitizers clean; multi-file program works
- [x] Review with the user before Phase 7

### Phase 7 - VM performance & limits (area 5)
- [x] T45 Raise limits (frames/locals/constants/jumps; 16-bit local opcodes, FRAMES_MAX 4096, MAX_LOCALS 1024)
- [x] T46 Growable value stack (reallocates 2048 to 4M items with pointer rebasing)
- [x] T47 GC: adaptive threshold scaling (1.5x - 2.5x) & `gc()` native
- [x] T47b Stable merge sort for `sort` / `sort_by` (follow-up from T9)
- [x] T48 Compiler constant folding + dead-code elimination
- [x] T49 Precompiled bytecode (`.sparkc`) + `--compile`
- [ ] T50 Native code generation / JIT (stretch - deferred)

### Checkpoint: Phase 7
- [x] Benchmark improved (~5.1 ms), all 148 tests + sanitizers clean
- [x] Review with the user before Phase 8

### Phase 8 - Tools & ecosystem (area 7)
- [x] T51 Debugger (`--debug`: breakpoints, step, locals)
- [x] T52 Formatter
- [x] T53 Linter
- [x] T54 CI + coverage + sanitizer job
- [x] T55 Concurrency (`spawn`/`yield`/channels) - delivered as Phase 14 (T81)

### Phase 9 - Catchable Runtime Errors & String Optimization
- [x] T56 Catchable runtime errors in `try`/`catch` (`ZeroDivisionError`, `TypeError`, `IndexError`, etc. caught as map)
- [x] T57 Amortized string concatenation (delivered as the `string_builder` API; `s += "x"` stays O(N^2) and is documented as such)

### Phase 10 - Standard Library Additions
- [x] T58 Math natives (`sin`, `cos`, `log`, `exp`, `pi`, `min`/`max` array support)
- [x] T59 Time natives (`time`, `sleep`)
- [x] T60 Text natives & unicode escapes (`chr`, `ord`, `pad_start`, `pad_end`, `\uXXXX`)
- [x] T61 Utility natives (`sum`, `any`, `all`, `enumerate`, `zip`, `isinstance`, `copy`)
- [x] T62 System & File natives (`list_dir`, `mkdir`, `exec`)

### Phase 11 - Negative Indices & Slices
- [x] T63 Negative indexing for arrays and strings (`a[-1]`)
- [x] T64 Slice native supporting negative bounds (`slice(str, start, end)`)
- [x] T65 Slice syntax `expr[start:end]` and `expr[start:end:step]`

### Phase 12 - Native Methods & Syntactic Sugar
- [x] T66 Methods on native types (`"abc".upper()`, `[1, 2].len()`, `str.split(sep)`)
- [x] T67 Increment / decrement operators (`i++`, `i--`, `++i`, `--i`)
- [x] T68 Short lambdas (`fn(x) => x * 2`)
- [x] T69 Binary `in` operator (`item in collection`)
- [x] T70 Power operator `**`, optional chaining `?.`, unary `+`
- [x] T71 Multiline strings (`"""..."""`)
- [x] T72 `for (var i, v in arr)` syntax
- [x] T73 Per-iteration loop binding for closures
- [x] T74 Destructuring (`var [a, b] = arr`, `var {x, y} = map`)

### Phase 13 - Full Classes & OOP
- [x] T75 First-class `ObjClass` and `ObjInstance` (`type(inst) == "ClassName"`)
- [x] T76 Inheritance with `class Sub extends Super` and `super(...)` / `super.method(...)`
- [x] T77 Class field declarations `var x = 3;` and `static` methods
- [x] T78 `instanceof` operator
- [x] T79 Custom string representation hook `to_string()`
- [x] T80 Operator overloading (`__add__`, `__sub__`, `__mul__`, `__div__`, `__index__`, etc.)

### Phase 14 - Concurrency (T55)
- [x] T81 Green-thread coroutines (`spawn`, `yield`, `channel`)

### Checkpoint: Complete
- [x] All acceptance criteria met
- [x] Full sanitizer + benchmark run (208 tests passing, ASan/UBSan clean, benchmark
      minimum 4.94 ms / median 5.01 ms measured on an idle machine)
- [x] Ready for review

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Integers destabilize the `double`-based VM, tests and benchmark | High | Dedicated phase with its own checkpoint; int/float promotion rules fixed in A1/A3; benchmark gate <= 5 ms |
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

**Phase 2 (syntax, T11-T18) is implemented.**

**Phase 3 (64-bit integers, T19-T26) is implemented.** `VAL_INT`/`int64_t` were added
in `src/value.h`, the lexer/parser produce integer literals, the VM promotes int/float
and detects overflow, and `div` plus the bitwise operators are in. The behavior is
covered by `tests/cases/{integers,bitwise}.spark` and
`tests/errors/{div_cero,div_no_entero,bitwise_no_entero,bitnot_no_entero,shift_fuera,int_conversion}.spark`,
with the reference in `README.md`, `docs/language.md` and `docs/architecture.md`.

**Phase 4 (standard library / I/O, T27-T33) is implemented.** `exit(code)`, `argv`,
`env(name?)`, `input(prompt?)`, one-shot file operations (`read_file`, `write_file`,
`append_file`, `file_exists`), file handles (`open`, `read_line`, `read`, `write`, `close`, `eof`),
PRNG (`rand`, `randint`, `srand`), JSON (`to_json`, `from_json`) and linear-time regex
(`regex_match`, `regex_find`, `regex_replace`) are in. Covered by
`tests/cases/{io,random,json,regex,fase4_e2e}.spark` and `tests/errors/{file_closed,from_json_error,randint_invalido,regex_invalido}.spark`.

**Phase 5 (functions & semantics, T34-T39) is implemented.** Tail-call
optimization, default parameters, varargs/spread, the REPL `_` value and `is_nan` are
in, with the review fixes listed in `tasks/todo.md`.

**Phase 6 (modules, exceptions, objects, T40-T44) is implemented.** `import` resolves
relative paths and caches modules by canonical path against the shared global namespace
(A6), `try`/`catch`/`finally` uses a VM handler stack that also catches runtime errors
as a `{type, message, line, column}` map, and the object model (classes, `new`,
inheritance, `super`, statics, `to_string`, operator overloading) landed in Phase 13.

**Phases 7-8 (performance, limits, tooling, T45-T54) are implemented.** Growable stacks,
`FRAMES_MAX 4096`/`MAX_LOCALS 1024`, adaptive GC thresholds with `gc()`, constant folding,
`.sparkc` bytecode, the debugger, formatter, linter and CI are all in.

**Phases 9-12 (T56-T74) are implemented.** Catchable runtime errors, slices,
lambdas, update operators, destructuring, native methods, optional chaining, multiline
strings and the extended native library are covered by the Phase 9-12 fixtures.

**Phase 13 (T75-T80) is implemented.** `ObjClass`/`ObjInstance`, inheritance, statics,
`instanceof`, `to_string` and operator overloading are covered by
`tests/cases/{oop_basics,methods_classes,oop_operators}.spark` and the `oop_*` error
fixtures.

**Phase 14 (T81) is implemented.** Cooperative coroutines and FIFO channels are in
(`spawn`, `yield`, `join`, `done`, `channel`, `send`, `receive`, `close`), with a private
value stack per coroutine, per-coroutine frame/try-handler snapshots and GC tracing of
blocked waiters. The bytecode format version is now 5 (`OP_YIELD`); see
`docs/language.md` section 15 and `tests/cases/{coroutines,coroutine_gc}.spark`.

## Open Questions (need human input)

Resolved:
- **Q1 (division) - DECIDED:** `/` stays float; integer division is spelled `div` (A3).
- **Q2 (`type`) - DECIDED:** keep `type(x) == "number"`; add `int()`/`float()`/`is_int()` (A2).
- **Q5 (objects) - DECIDED:** method sugar on maps first; full classes deferred (A7).
- **Q8 (priority) - DEFERRED:** plan only for now; the user will give the go-ahead
  before implementation starts.
- **Q9 (tracker) - CONFIRMED:** tasks live in `tasks/todo.md`.

- **Q3 (modules) - DECIDED AND IMPLEMENTED:** relative paths resolve against the
  importing file and every module runs once against the shared global namespace (A6).
- **Q4 (exceptions) - DECIDED AND IMPLEMENTED:** `try { } catch (e) { } finally { }`;
  `e` is a map with `type`, `message`, `line` and `column`; an uncaught value keeps the
  previous stderr/exit behavior.
- **Q6 (concurrency) - DECIDED AND IMPLEMENTED:** VM green threads (`spawn`/`yield`/
  channels), single-threaded and cooperative (A8), delivered in Phase 14.
- **Q7 (breaking changes) - DECIDED:** yes, and they are recorded in `CHANGELOG.md`
  under "Breaking changes".
- **Q10 (JIT) - DEFERRED:** T50 stays a stretch goal; the interpreter with fused
  superinstructions is the supported engine.

## Baseline (for regression checks)

- Tests: 207 passing (`python3 tests/run_tests.py`), also clean under ASan/UBSan.
- Benchmark: `fib(27)` minimum 4.94 ms, median 5.01 ms on an idle machine
  (`python3 benchmarks/run.py`; a few percent of variance under load).
- No warnings with `-O3 -Wall -Wextra -Wpedantic`.

## Known gaps

- None open. `s += "x"` in a loop is deliberately O(N^2) (measured: 200,000 appends cost
  ~2.2 s, versus ~6 ms through `string_builder`); the rationale and the rejected designs
  are recorded in `docs/architecture.md`, and the builder is guarded by
  `tests/cases/string_builder_scale.spark`.