# AGENTS.md

Guidance for AI agents working on the **Chispa** project.

## Language policy

- **Project artifacts are English-only.** Code comments, documentation, example
  programs, user-facing messages (CLI, REPL, runtime errors, test output) and commit
  messages must be written in English.
- **Talking to the user may be in Spanish.** The user prefers Spanish for the
  conversation, so replies can be in Spanish even though the artifacts stay English.
- Whenever you touch a Spanish string or comment, translate it to English. Do not
  introduce new Spanish text in the artifacts.
- Code identifiers already in Spanish are part of the current API; prefer keeping them
  and only translate prose (comments and messages) unless a rename is explicitly
  requested.

## What this project is

Chispa is an interpreted programming language written from scratch in C11 with **no
external dependencies**. The pipeline is:

```
source -> lexer.c -> parser.c (AST) -> compiler.c (bytecode) -> vm.c (stack VM + GC)
```

Everything lives in `src/`.

## Build and test

On Windows there is no C compiler: build and test from WSL (Ubuntu).

```bash
make                       # builds ./chispa
make test                  # runs the test suite (needs python3)
python3 benchmarks/run.py  # runs the fib(27) benchmark (reports min/median)
```

Sanitizers:

```bash
make clean
make CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic" \
     LDFLAGS="-fsanitize=address,undefined"
python3 tests/run_tests.py
make clean && make
```

## Conventions

- C11 with `-O3 -Wall -Wextra -Wpedantic`. The build must be warning-free.
- No external dependencies and no parser/VM generators: everything is handwritten.
- Match the existing structure, naming, and style of the surrounding code.
- Reuse the shared binary-operation helpers in `src/vm.c` (`compare_values`,
  `arithmetic_values`, `add_values`) instead of duplicating logic.
- Adding an opcode means: add it to the enum in `src/chunk.h`, handle it in
  `opcode_name` and the disassembler in `src/chunk.c`, implement it in `run()` in
  `src/vm.c`, and add a test fixture.
- Keep the stack balanced: every path must leave the operand stack exactly as the
  instruction contract requires.
- Always measure VM changes with `python3 benchmarks/run.py`; never guess.
- Tests: `tests/cases` holds valid programs compared against exact `.out` output;
  `tests/errors` holds failing programs with `exit:`/`stderr:` specifications.
- Run the whole suite (`python3 tests/run_tests.py`) before considering a change done.

## Performance target

`benchmarks/fib.spark` computes `fib(27)`. The goal is a minimum time in the
**10-15 ms** range on the development machine (currently ~13 ms). The VM amortizes
dispatch with fused superinstructions; avoid regressing that.