# AGENTS.md

Operating guide for AI agents (and automated collaborators) working on the
**Spark** repository. Read it before making any change: it defines the project's
ground rules, its definition of done and the conventions of the codebase.

## 1. What this project is

Spark is an interpreted programming language implemented from scratch in C11. The
pipeline is:

```
source -> lexer.c -> parser.c (AST) -> compiler.c (bytecode) -> vm.c (stack VM + GC)
```

Everything lives in `src/`. Spark has **no external dependencies except OpenSSL**,
which is linked only for the TLS transport (`tls_connect` and `https://`); the
socket, HTTP and WebSocket layers stay dependency-free. This is the single
approved exception (decision Q11); see [`docs/architecture.md`](docs/architecture.md)
and [`CHANGELOG.md`](CHANGELOG.md).

Current status is tracked in [`tasks/todo.md`](tasks/todo.md) and
[`tasks/plan.md`](tasks/plan.md). Consult them before starting work to see what is
active, what is complete and which decisions are already made.

## 2. Environment

On Windows there is no C compiler: build and test from WSL (Ubuntu). The build
requires the OpenSSL development files (`libssl-dev` on Debian/Ubuntu), a C11
compiler, `make`, `python3` and the math library. OpenSSL is the only third-party
dependency.

## 3. Build and test

```bash
make                       # builds ./spark
make test                  # runs the test suite (needs python3)
python3 benchmarks/run.py  # runs the fib(27) benchmark (reports min/median)
```

Sanitizer run:

```bash
make clean
make CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic" \
     LDFLAGS="-fsanitize=address,undefined"
python3 tests/run_tests.py
make clean && make
```

Run the whole suite (`python3 tests/run_tests.py`) before considering a change
done. The sanitizer run is required for any change to the VM, the GC or the
compiler.

## 4. Definition of done

Every task must satisfy all of the following:

- The build is warning-free under `-O3 -Wall -Wextra -Wpedantic`.
- `python3 tests/run_tests.py` passes, and new behavior has new fixtures.
- The sanitizer run is clean when the change touches the VM, GC or compiler.
- `python3 benchmarks/run.py` keeps the `fib(27)` minimum **below 5 ms** unless the
  task is explicitly about performance (in which case the target is stated).
- Documentation is updated where behavior changes: [`README.md`](README.md),
  [`docs/language.md`](docs/language.md), [`docs/architecture.md`](docs/architecture.md).

## 5. Conventions

- C11 with `-O3 -Wall -Wextra -Wpedantic`. The build must be warning-free.
- No external dependencies except OpenSSL for TLS; no parser or VM generators:
  everything else is handwritten.
- Match the existing structure, naming and style of the surrounding code.
- Reuse the shared binary-operation helpers in `src/vm.c` (`compare_values`,
  `arithmetic_values`, `add_values`) instead of duplicating logic.
- Adding an opcode means: add it to the enum in `src/chunk.h`, handle it in
  `opcode_name` and the disassembler in `src/chunk.c`, implement it in `run()` in
  `src/vm.c`, and add a test fixture.
- Keep the stack balanced: every path must leave the operand stack exactly as the
  instruction contract requires.
- Always measure VM changes with `python3 benchmarks/run.py`; never guess.
- Tests: [`tests/cases`](tests/cases) holds valid programs compared against exact
  `.out` output; [`tests/errors`](tests/errors) holds failing programs with
  `exit:`/`stderr:` specifications.
- The certificates under [`tests/tls/`](tests/tls) are throwaway fixtures. Their
  private keys are public and must never be trusted outside the test suite;
  regenerate them with `tests/tls/generate.sh`.
- Test fixtures must never touch the public internet: use local servers and
  ephemeral ports.

## 6. Performance target

`benchmarks/fib.spark` computes `fib(27)`. On an idle development machine the
minimum is around 4.9 ms and the median around 5.1 ms, and the gate set in
[`tasks/plan.md`](tasks/plan.md) is a minimum **below 5 ms**. The VM amortizes
dispatch with fused superinstructions; avoid regressing that.

## 7. Useful references

- [`README.md`](README.md) - overview, features, build, usage and the native reference.
- [`docs/language.md`](docs/language.md) - the language reference and error behavior.
- [`docs/architecture.md`](docs/architecture.md) - internals, the GC model and the scheduler.
- [`tasks/plan.md`](tasks/plan.md) / [`tasks/todo.md`](tasks/todo.md) - the roadmap and its status.
- [`CHANGELOG.md`](CHANGELOG.md) - released and unreleased changes.
