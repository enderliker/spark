# Spark task list

Detailed, dependency-ordered task list for the roadmap in `tasks/plan.md`. Execute
top to bottom; do not start a phase before its checkpoint is approved. Every task
must satisfy the Definition of Done in the plan (warning-free build, tests, docs,
sanitizers for VM tasks, benchmark <= 15 ms).

Legend: **scope** S (1-2 files), M (3-5), L (5-8). Anything XL is split further.

---

## Phase 1 - Collections & string library (area 2)

### T1 - Array search natives (`contains`, `index_of`)
**Description:** Add `contains(array, value)` and `index_of(array, value)` (returns -1
when absent) using the existing `values_equal`.
**Acceptance criteria:**
- [x] `contains` returns a bool; `index_of` returns the first index or -1.
- [x] Works for mixed types and object identity/string content per `values_equal`.
- [x] Wrong first argument raises a normal runtime error.
**Verification:**
- [x] `make` clean; new fixture in `tests/cases`; `python3 tests/run_tests.py` passes.
- [x] `docs/language.md` native table updated.
**Dependencies:** None.
**Files:** `src/vm.c`, `tests/cases/collections.spark`, `tests/cases/collections.out`, `docs/language.md`.
**Scope:** S.

### T2 - Array mutation natives (`insert`, `remove`, `reverse`, `fill`)
**Description:** `insert(array, index, value)`, `remove(array, index)` (returns the
removed value), `reverse(array)` (in place, returns the array), `fill(array, value)`.
**Acceptance criteria:**
- [x] Bounds/validity errors for invalid indexes.
- [x] `reverse` and `fill` mutate in place and return the array.
**Verification:**
- [x] Fixtures + `python3 tests/run_tests.py`; docs updated.
**Dependencies:** T1 (same fixture file).
**Files:** `src/vm.c`, `tests/cases/collections.*`, `docs/language.md`.
**Scope:** S.

### T3 - Array `slice`
**Description:** `slice(array, start, end?)` returns a new array with `[start, end)`.
**Acceptance criteria:**
- [x] Negative or out-of-range handling documented and tested; default `end` = length.
- [x] Returns a new array (original untouched).
**Verification:**
- [x] Fixtures + suite; docs updated.
**Dependencies:** T1.
**Files:** `src/vm.c`, `tests/cases/collections.*`, `docs/language.md`.
**Scope:** S.

### T4 - String search natives
**Description:** `contains`, `index_of` (by character), `starts_with`, `ends_with` for
strings.
**Acceptance criteria:**
- [x] Positions are in UTF-8 characters, not bytes.
- [x] Works with empty needles/substrings per documented rules.
**Verification:**
- [x] Fixtures (including `ñandú` cases); suite; docs.
**Dependencies:** None.
**Files:** `src/vm.c`, `tests/cases/strings.spark`, `tests/cases/strings.out`, `docs/language.md`.
**Scope:** S.

### T5 - String transform natives
**Description:** `split`, `join`, `trim`, `upper`, `lower`, `repeat`, `replace`.
**Acceptance criteria:**
- [x] `split(text, sep)` -> array; `join(array, sep)` -> text (elements stringified).
- [x] `upper`/`lower` are ASCII-only (documented) to stay dependency-free.
- [x] `replace(text, old, new)` replaces all occurrences.
**Verification:**
- [x] Fixtures; suite; docs; sanitizers (allocates objects).
**Dependencies:** T4 (same fixture file).
**Files:** `src/vm.c`, `tests/cases/strings.*`, `docs/language.md`.
**Scope:** M.

### T6 - Map helpers (`clear`, `merge`, `get` with default)
**Description:** `clear(map)`, `merge(dest, src)` (src wins, returns dest),
`get(map, key, default)`.
**Acceptance criteria:**
- [x] `get` returns the default only when the key is absent (not when present with `nil`).
**Verification:**
- [x] Fixtures; suite; docs; sanitizers.
**Dependencies:** None.
**Files:** `src/vm.c`, `tests/cases/collections.*`, `docs/language.md`.
**Scope:** S.

### T7 - Native callback support (re-entrant call) - design + implementation
**Description:** A `vm_call_value_for_native(Value callee, int argc)` helper that runs a
Spark function to completion from inside a native without corrupting the current
frame, plus a spike documenting the approach (nested loop vs frame push).
**Acceptance criteria:**
- [x] A test native (e.g. an internal one used by T8) can call a closure and get its
      result; errors inside the callback propagate correctly.
- [x] Stack/frames are restored exactly; sanitizers clean.
**Verification:**
- [x] `make`; suite; ASan/UBSan build; a focused fixture calling a closure from a native.
- [x] Short design note in `docs/architecture.md`.
**Dependencies:** None (blocks T8, T9).
**Files:** `src/vm.c`, `docs/architecture.md`, `tests/cases/*`.
**Scope:** M (risky - do the spike first).

### T8 - Higher-order natives (`map`, `filter`, `reduce`, `each`)
**Description:** `map(array, fn)`, `filter(array, fn)`, `reduce(array, fn, initial?)`,
`each(array, fn)` using T7.
**Acceptance criteria:**
- [x] `reduce` semantics documented (with and without initial value).
- [x] Callback arity validated with a clear error.
**Verification:**
- [x] Fixtures; suite; sanitizers.
**Dependencies:** T7.
**Files:** `src/vm.c`, `tests/cases/higher_order.*`, `docs/language.md`.
**Scope:** M.

### T9 - `sort` / `sort_by`
**Description:** `sort(array)` (default comparison for numbers/strings) and
`sort_by(array, fn)` using a callback comparator.
**Acceptance criteria:**
- [x] Stable-ish behavior documented; returns the sorted array (in place).
- [x] Comparator contract documented (boolean "a before b" or -1/0/1; pick one).
**Verification:**
- [x] Fixtures; suite; sanitizers; benchmark unaffected.
**Dependencies:** T7.
**Files:** `src/vm.c`, `tests/cases/collections.*`, `docs/language.md`.
**Scope:** M.

### T10 - Docs + tests for the new natives
**Description:** Consolidate the native reference and add missing fixtures/edge cases.
**Acceptance criteria:**
- [x] Native table in README + language.md is complete and consistent with the code.
- [x] Each new native has at least one positive and one error fixture.
**Verification:**
- [x] `python3 tests/run_tests.py`; no untested native (grep cross-check).
**Dependencies:** T1-T9.
**Files:** `README.md`, `docs/language.md`, `tests/`.
**Scope:** S.

## Checkpoint: Phase 1
- [x] All tests pass, build clean, docs updated
- [x] Review with the user before Phase 2

---

## Phase 2 - Language syntax (area 1)

### T11 - `const` declarations
**Description:** `const name = value;` - compile-time error on reassignment or missing
initializer; `const` is not required to be a compile-time constant, just immutable.
**Acceptance criteria:**
- [x] Reassigning a const (direct or compound) is a compile error with a clear message.
- [x] `const` works for locals and globals.
**Verification:**
- [x] Valid fixture + error fixture; suite; docs.
**Dependencies:** None.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T12 - Ternary operator `?:`
**Description:** Right-associative `cond ? a : b`, lazily evaluating only one branch.
**Acceptance criteria:**
- [x] Only the taken branch is evaluated (observable via side effects).
- [x] Precedence documented between `or`/`and` and assignment.
**Verification:**
- [x] Fixture with side effects; suite; docs.
**Dependencies:** None.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T13 - Null-coalescing `??`
**Description:** `a ?? b` returns `a` unless it is `nil`, in which case `b`.
**Acceptance criteria:**
- [x] Right operand is only evaluated when the left is `nil`.
**Verification:**
- [x] Fixture; suite; docs.
**Dependencies:** T12 (shares lexer/parser edits).
**Files:** same as T12.
**Scope:** S.

### T14 - `do { } while`
**Description:** `do { body } while (cond);` runs the body at least once.
**Acceptance criteria:**
- [x] `break`/`continue` work as in `while`.
**Verification:**
- [x] Fixture; suite; docs.
**Dependencies:** None.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `tests/`, `docs/language.md`.
**Scope:** S.

### T15 - Labelled loops + labelled `break`/`continue`
**Description:** `outer: for (...) { ... break outer; }`.
**Acceptance criteria:**
- [ ] Breaking/continuing a non-existent label is a compile error.
- [ ] Stack cleanup is correct when breaking out of nested scopes.
**Verification:**
- [ ] Fixture with nested loops; suite; sanitizers; docs.
**Dependencies:** None.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T16 - `switch`/`case`/`default`
**Description:** `switch (expr) { case v: ...; default: ...; }` with fall-through or
auto-break (decide; propose auto-break for safety) .
**Acceptance criteria:**
- [ ] Semantics documented precisely (fall-through vs auto-break).
- [ ] Works with numbers and strings (and maps on equality).
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** None.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T17 - String interpolation
**Description:** `"hello ${name}, x=${1 + 2}"` compiles to concatenation.
**Acceptance criteria:**
- [ ] `${...}` accepts arbitrary expressions; `$$` escapes a literal `$`.
- [ ] Existing escapes still work.
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** None.
**Files:** `src/lexer.c`, `src/parser.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T18 - Numeric literal sugar
**Description:** Hex (`0x`), binary (`0b`) and `_` digit separators.
**Acceptance criteria:**
- [x] Values match the decimal equivalent; invalid forms are syntax errors.
**Verification:**
- [x] Fixtures; suite; docs.
**Dependencies:** None.
**Files:** `src/lexer.c`, `tests/`, `docs/language.md`.
**Scope:** S.

## Checkpoint: Phase 2
- [ ] All tests pass, build clean, docs updated
- [ ] Review with the user before Phase 3

---

## Phase 3 - 64-bit integers (area 2 core)

### T19 - `VAL_INT`: value helpers, formatting, equality
**Description:** Add `VAL_INT` + `int64_t`, `INT_VAL`, `AS_INT`, `is_int`; format with
`%lld`; equality compares exactly and across int/number when values are equal.
**Acceptance criteria:**
- [ ] `values_equal` treats `1` and `1.0` as equal; hashing is consistent with equality.
- [ ] `value_print`/`str` produce integers without a decimal point.
**Verification:**
- [ ] Unit-ish fixtures; suite; sanitizers.
**Dependencies:** None (start of Phase 3).
**Files:** `src/value.h`, `src/object.c`, `tests/`.
**Scope:** M.

### T20 - Integer literals in lexer/parser
**Description:** Integer literals without `.`/exponent become `VAL_INT`; keep `double`
for `3.0`, `1e3`.
**Acceptance criteria:**
- [ ] Lexer no longer routes plain integers through `double`.
- [ ] Existing numeric output unchanged (`str(42)` == `"42"`).
**Verification:**
- [ ] Suite (existing tests must still pass); docs.
**Dependencies:** T19.
**Files:** `src/lexer.c`, `src/ast.h`, `src/parser.c`, `src/compiler.c`, `tests/`.
**Scope:** M.

### T21 - Arithmetic & comparison with int/float promotion
**Description:** `+ - *` stay int when both are int and no overflow, else promote to
`double`; `< <= > >=` compare across types by promoting.
**Acceptance criteria:**
- [ ] Overflow promotes deterministically to `double` (documented).
- [ ] Mixed comparisons are correct (e.g. `1 < 1.5`).
**Verification:**
- [ ] Fixtures incl. overflow cases; suite; sanitizers; `python3 benchmarks/run.py` <= 15 ms.
**Dependencies:** T19, T20.
**Files:** `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M (risky).

### T22 - Integer division/modulo (`//`, `%`) per A3
**Description:** Add `//` (truncating integer division) and define `%` for ints. `/`
stays floating-point division (decision A3).
**Acceptance criteria:**
- [ ] Semantics for negatives documented and tested (`-7 // 2`, `-7 % 2`).
- [ ] Division by zero is still a runtime error.
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** T21.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/compiler.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T23 - Integer map hashing + integer array indexes
**Description:** Hash `VAL_INT` consistently with `VAL_NUMBER` so `m[1]` and `m[1.0]`
hit the same entry; array indexes accept int and integral floats.
**Acceptance criteria:**
- [ ] `map[1]` and `map[1.0]` resolve to the same key.
- [ ] Index validation messages unchanged/updated and tested.
**Verification:**
- [ ] Fixtures; suite; sanitizers.
**Dependencies:** T19.
**Files:** `src/object.c`, `src/vm.c`, `tests/`.
**Scope:** M.

### T24 - Integer-aware natives
**Description:** `int(x)`, `float(x)`, `is_int(x)`; make `abs`/`min`/`max`/`floor`/`ceil`/
`round` int-preserving where sensible.
**Acceptance criteria:**
- [ ] Rounding/abs return ints for int inputs (documented).
- [ ] `int("12")`/`int(3.9)` behavior documented.
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** T19.
**Files:** `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T25 - Bitwise operators
**Description:** `& | ^ ~ << >>` (and compound `&= |= ^= <<= >>=`); integer-only with a
runtime error otherwise.
**Acceptance criteria:**
- [ ] Shift semantics for negative/oversized shifts documented; no UB.
- [ ] Non-integer operands produce a clear runtime error.
**Verification:**
- [ ] Fixtures; suite; sanitizers (UB is the main risk); docs.
**Dependencies:** T19, T21.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `src/chunk.h`, `src/chunk.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** L (split into lexer/parser then VM if needed).

### T26 - Docs + tests for integers
**Description:** Complete the integer documentation and edge-case fixtures.
**Acceptance criteria:**
- [ ] language.md documents promotion, overflow, division and bitwise rules.
- [ ] architecture.md notes `VAL_INT` in the value model.
**Verification:**
- [ ] Suite; benchmark gate; sanitizers.
**Dependencies:** T19-T25.
**Files:** `README.md`, `docs/language.md`, `docs/architecture.md`, `tests/`.
**Scope:** S.

## Checkpoint: Phase 3 (critical)
- [ ] All tests pass, sanitizers clean
- [ ] Benchmark minimum still <= 15 ms
- [ ] Review with the user before Phase 4

---

## Phase 4 - Standard library / I/O (area 4)

### T27 - `exit(code)`, `argv`, `env(name?)`
**Description:** `exit(code)` stops with that status; `argv` is a global array of the
script arguments; `env` reads environment variables.
**Acceptance criteria:**
- [ ] `exit` flushes the REPL/stdout and returns the requested code.
- [ ] `argv` includes the script name and its arguments; empty in the REPL.
**Verification:**
- [ ] CLI fixture; suite; docs.
**Dependencies:** None.
**Files:** `src/main.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T28 - stdin (`input(prompt?)`)
**Description:** `input(prompt?)` reads one line from stdin (without the newline).
**Acceptance criteria:**
- [ ] Returns `nil` on EOF.
- [ ] Prints the prompt when provided and flushes.
**Verification:**
- [ ] Fixture feeding stdin via the test harness; suite; docs.
**Dependencies:** T27 (shares main/vm wiring).
**Files:** `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** S.

### T29 - One-shot file natives
**Description:** `read_file(path)`, `write_file(path, data)`, `append_file(path, data)`,
`file_exists(path)`; errors as runtime errors.
**Acceptance criteria:**
- [ ] Missing/unreadable files produce a runtime error with the path.
- [ ] `write_file` truncates, `append_file` appends.
**Verification:**
- [ ] Fixture using `/tmp` files; suite; docs.
**Dependencies:** None.
**Files:** `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** S.

### T30 - File object
**Description:** `open(path, mode)` returns a file handle with `read_line`, `read`,
`write`, `close`, `eof`.
**Acceptance criteria:**
- [ ] A new `ObjFile` type is GC-managed and closed on finalization.
- [ ] Using a closed handle is a clear runtime error.
**Verification:**
- [ ] Fixture; suite; sanitizers (fd leak check); docs.
**Dependencies:** T29.
**Files:** `src/value.h`, `src/object.c/h`, `src/vm.c`, `tests/`, `docs/`.
**Scope:** M (risky: new object type + GC).

### T31 - Random (`rand`, `randint`, `srand`)
**Description:** Dependency-free PRNG (e.g. xorshift/PCG) with explicit seeding.
**Acceptance criteria:**
- [ ] Deterministic given a seed (documented range for `randint`).
- [ ] Reproducible fixtures (fixed seed).
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** T19 (integers).
**Files:** `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** S.

### T32 - JSON encode/decode
**Description:** `to_json(value)` and `from_json(text)` with a documented mapping
(nil/bool/int/number/string/array/map).
**Acceptance criteria:**
- [ ] Round-trip of supported values; unsupported values raise an error.
- [ ] Malformed JSON raises a runtime error with position info.
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** T19 (ints), T5 (strings).
**Files:** `src/object.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T33 - Regex (stretch)
**Description:** A small backtracking regex engine (`match`, `find`, `replace`) or
document why it is deferred.
**Acceptance criteria:**
- [ ] Supported syntax subset documented.
- [ ] ReDoS risk acknowledged (complexity limits).
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** T5.
**Files:** `src/vm.c` (or a new `src/regex.c/.h`), `tests/`, `docs/`.
**Scope:** L (stretch - may be deferred).

## Checkpoint: Phase 4
- [ ] All tests pass; an end-to-end script (read file, parse, write output) works
- [ ] Review with the user before Phase 5

---

## Phase 5 - Functions & semantics (areas 3, 6)

### T34 - Tail-call optimization
**Description:** Reuse the frame for `return f(args);` when the call is in tail position.
**Acceptance criteria:**
- [ ] Deep tail recursion (e.g. 1e6) does not exhaust frames.
- [ ] Non-tail recursion still errors at the frame limit.
**Verification:**
- [ ] Fixture; suite; sanitizers; docs note.
**Dependencies:** None.
**Files:** `src/compiler.c`, `src/vm.c`, `tests/`, `docs/architecture.md`.
**Scope:** M.

### T35 - Default parameters
**Description:** `fn f(a, b = 10)` - missing trailing args use the default expression
(evaluated per call).
**Acceptance criteria:**
- [ ] Calling with fewer args works; calling with too many still errors.
- [ ] Defaults can reference earlier parameters.
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** None.
**Files:** `src/parser.c`, `src/ast.h`, `src/compiler.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T36 - Varargs + spread (`...`)
**Description:** `fn f(...rest)` collects extra args into an array; `f(...array)` spreads.
**Acceptance criteria:**
- [ ] Varargs and spread can coexist with fixed params.
- [ ] Arity checks account for varargs.
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** T35 (shares parser/compiler edits).
**Files:** `src/parser.c`, `src/ast.h`, `src/compiler.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T37 - Named arguments (optional)
**Description:** `f(a: 1, b: 2)` with compile-time checks against parameter names.
**Acceptance criteria:**
- [ ] Mixing positional and named is allowed only in the documented order.
- [ ] Unknown/duplicate names are compile errors.
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** T35.
**Files:** `src/parser.c`, `src/ast.h`, `src/compiler.c`, `tests/`, `docs/language.md`.
**Scope:** M (optional - may be deferred).

### T38 - REPL `_` last value
**Description:** Expose the previous expression result as `_` in the REPL.
**Acceptance criteria:**
- [ ] `_` is set after each expression statement; documented.
**Verification:**
- [ ] REPL session in `tests/run_tests.py`; docs.
**Dependencies:** None.
**Files:** `src/main.c`, `src/vm.c`, `tests/run_tests.py`, `docs/language.md`.
**Scope:** S.

### T39 - Numeric/semantics polish
**Description:** `is_nan(x)`, documented NaN equality, and any equality edge cases
found while adding integers.
**Acceptance criteria:**
- [ ] NaN behavior documented and tested; `is_nan` available.
**Verification:**
- [ ] Fixtures; suite; docs.
**Dependencies:** T19.
**Files:** `src/object.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** S.

## Checkpoint: Phase 5
- [ ] All tests pass, build clean, docs updated
- [ ] Review with the user before Phase 6

---

## Phase 6 - Modules, exceptions, objects

### T40 - `import` design + implementation
**Description:** `import "path";` compiles and runs a module once per resolved path;
propose a shared global namespace (A6).
**Acceptance criteria:**
- [ ] Relative paths resolve against the importing file.
- [ ] A module's top-level `var`/`fn` become usable by the importer (per Q3).
**Verification:**
- [ ] Multi-file fixture; suite; docs.
**Dependencies:** Q3 answered.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/compiler.c`, `src/vm.c`, `src/main.c`, `tests/`, `docs/`.
**Scope:** L (do the design note first).

### T41 - Module cache + cyclic-import handling
**Description:** Cache compiled modules by absolute path; detect/handle import cycles.
**Acceptance criteria:**
- [ ] Importing the same module twice runs it once.
- [ ] Cycles either error clearly or are handled (documented).
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** T40.
**Files:** `src/vm.c`, `tests/`, `docs/architecture.md`.
**Scope:** M.

### T42 - `throw` + `try`/`catch`/`finally`
**Description:** Exception handler stack in the VM; `OP_THROW` unwinds to the nearest
handler; uncaught throw is a runtime error. Caught value per Q4.
**Acceptance criteria:**
- [ ] `finally` always runs; stack/upvalues are cleaned during unwinding.
- [ ] Errors inside `catch`/`finally` behave correctly.
**Verification:**
- [ ] Fixtures incl. nested try; suite; sanitizers; docs.
**Dependencies:** Q4 answered.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/chunk.h`, `src/chunk.c`, `src/compiler.c`, `src/vm.c`, `tests/`, `docs/`.
**Scope:** L (do the design note first).

### T43 - Method-call sugar on maps + `self`
**Description:** `recv.method(args)` looks up `"method"` in a map and calls it with the
receiver bound to `self`.
**Acceptance criteria:**
- [ ] Calling a method on a non-map/missing key errors clearly.
- [ ] `self` is available inside the method.
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** None.
**Files:** `src/parser.c`, `src/ast.h`, `src/compiler.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T44 - Classes (optional/advanced)
**Description:** Full `class`/`new`/fields/methods and (optionally) single inheritance.
**Acceptance criteria:**
- [ ] Per Q5; fields, methods, `self`, and construction documented.
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** T43, Q5 answered.
**Files:** `src/lexer.c/h`, `src/parser.c`, `src/ast.h`, `src/compiler.c`, `src/vm.c`, `src/object.c/h`, `tests/`, `docs/`.
**Scope:** L (design note first; may be deferred).

## Checkpoint: Phase 6
- [ ] All tests pass, sanitizers clean; a multi-file program works
- [ ] Review with the user before Phase 7

---

## Phase 7 - VM performance & limits (area 5)

### T45 - Raise limits (32-bit operands where needed)
**Description:** Increase `FRAMES_MAX`, `MAX_LOCALS`, constant count and jump range;
widen operands (e.g. 32-bit constants/jumps) where required.
**Acceptance criteria:**
- [ ] New caps documented and tested; disassembler updated for wider operands.
- [ ] No silent truncation.
**Verification:**
- [ ] Fixtures at the new boundary; suite; benchmark <= 15 ms; docs.
**Dependencies:** None.
**Files:** `src/chunk.h`, `src/chunk.c`, `src/compiler.c`, `src/vm.c`, `tests/`, `docs/architecture.md`.
**Scope:** M (risk: operand layout change).

### T46 - Growable value stack
**Description:** Replace the fixed `stack[STACK_MAX]` with a growable buffer (or raise
it and detect overflow safely).
**Acceptance criteria:**
- [ ] Deep but finite recursion beyond the old stack size works when frames allow.
- [ ] A controlled error instead of UB when memory is exhausted.
**Verification:**
- [ ] Fixture; suite; sanitizers; docs.
**Dependencies:** T45.
**Files:** `src/vm.c`, `src/memory.c`, `tests/`, `docs/architecture.md`.
**Scope:** M (risk: pointers into the stack must stay valid).

### T47 - GC: incremental or generational
**Description:** Reduce stop-the-world pauses (incremental marking or a nursery).
**Acceptance criteria:**
- [ ] Correctness preserved (no premature frees); sanitizer clean.
- [ ] A GC-heavy fixture shows bounded pause behavior (documented measurement).
**Verification:**
- [ ] `tests/cases/memoria*`; suite; sanitizers; docs.
**Dependencies:** None.
**Files:** `src/vm.c`, `src/object.c`, `src/memory.c`, `tests/`, `docs/architecture.md`.
**Scope:** L (risky).

### T47b - Stable merge sort for `sort` / `sort_by`
**Description:** Replace the O(n^2) insertion sorts added in T9 with a stable merge sort
that reuses the same comparator contract (`sort_by`'s `fn(x, y)` returns whether `x`
goes before `y`).
**Acceptance criteria:**
- [ ] `sort`/`sort_by` remain stable and keep the documented comparator contract.
- [ ] Large arrays (e.g. 100k elements) sort in O(n log n); benchmark unaffected.
- [ ] Callback errors still propagate and the stack stays balanced.
**Verification:**
- [ ] Fixtures with large arrays; suite; sanitizers; `python3 benchmarks/run.py` <= 15 ms.
**Dependencies:** T9, T46 (growable stack, optional).
**Files:** `src/vm.c`, `tests/cases/sorting.*`, `docs/language.md`.
**Scope:** S.

### T48 - Compiler constant folding + dead-code elimination
**Description:** Fold constant arithmetic/comparisons at compile time; drop code after
`return`/`break`/`continue`.
**Acceptance criteria:**
- [ ] Folding preserves semantics (including errors at compile vs runtime where safe).
- [ ] `--dump` shows folded constants.
**Verification:**
- [ ] Fixtures; suite; benchmark improvement measured; docs.
**Dependencies:** T21 (for int folding).
**Files:** `src/compiler.c`, `tests/`, `docs/architecture.md`.
**Scope:** M.

### T49 - Precompiled bytecode (`.sparkc`) + `--compile`
**Description:** Serialize/deserialize an `ObjFunction` to disk and load it, with a
version/magic guard.
**Acceptance criteria:**
- [ ] Loading a compiled file runs identically to the source.
- [ ] Corrupt/old files error clearly.
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** T45 (stable operand layout).
**Files:** `src/chunk.c`, `src/object.c`, `src/main.c`, `tests/`, `docs/architecture.md`.
**Scope:** L (design note first).

### T50 - Native code generation / JIT (stretch)
**Description:** Optional baseline JIT or an ahead-of-time C generator from bytecode.
**Acceptance criteria:**
- [ ] Per Q10; scoped minimal subset; documented limitations.
**Verification:**
- [ ] Correctness on the existing suite; benchmark comparison; docs.
**Dependencies:** T49.
**Files:** new `src/jit.*` (or a codegen tool), `tests/`, `docs/`.
**Scope:** XL (stretch - likely deferred).

## Checkpoint: Phase 7
- [ ] Benchmark improved, tests + sanitizers clean
- [ ] Review with the user before Phase 8

---

## Phase 8 - Tools & ecosystem (area 7)

### T51 - Debugger (`--debug`)
**Description:** Breakpoints, single-step, and locals inspection over the existing
`--dump`/line info.
**Acceptance criteria:**
- [ ] `break file:line`, `step`, `locals`, `continue` commands.
- [ ] Does not affect normal execution.
**Verification:**
- [ ] Scripted debug session fixture; suite; docs.
**Dependencies:** T49 (line info stability) optional.
**Files:** `src/main.c`, `src/vm.c`, `tests/`, `docs/`.
**Scope:** M.

### T52 - Formatter
**Description:** `spark --format file` canonicalizes indentation/spacing.
**Acceptance criteria:**
- [ ] Idempotent (formatting twice is stable); does not change semantics.
**Verification:**
- [ ] Round-trip fixtures; suite; docs.
**Dependencies:** None.
**Files:** `src/main.c`, new formatter module, `tests/`, `docs/`.
**Scope:** M.

### T53 - Linter
**Description:** `spark --lint file` reports unused variables, unreachable code and
shadowing.
**Acceptance criteria:**
- [ ] Exit code signals findings; each rule documented.
- [ ] No false positives on the example programs.
**Verification:**
- [ ] Fixtures with known findings; suite; docs.
**Dependencies:** T48 (unreachable code) optional.
**Files:** new linter module, `src/compiler.c` (reuse scope info), `tests/`, `docs/`.
**Scope:** M.

### T54 - CI + coverage + sanitizer job
**Description:** A CI workflow that builds, runs the suite, a sanitizer build and the
benchmark; publish coverage.
**Acceptance criteria:**
- [ ] CI runs on push; benchmark regression check included.
- [ ] Documented how to run locally.
**Verification:**
- [ ] CI green on the current tree; docs.
**Dependencies:** None.
**Files:** `.github/workflows/ci.yml`, `README.md` (or project CI files).
**Scope:** S.

### T55 - Concurrency (`spawn`/`yield`/channels) - stretch
**Description:** VM green-thread coroutines with `spawn`, `yield` and channels, per Q6.
**Acceptance criteria:**
- [ ] Cooperative scheduling; deterministic test with channels.
- [ ] GC handles suspended coroutine stacks.
**Verification:**
- [ ] Fixtures; suite; sanitizers; docs.
**Dependencies:** Q6 answered.
**Files:** `src/vm.c`, `src/object.c/h`, `tests/`, `docs/`.
**Scope:** XL (stretch - likely deferred).

## Checkpoint: Complete
- [ ] All acceptance criteria met
- [ ] Full sanitizer + benchmark run
- [ ] Ready for review