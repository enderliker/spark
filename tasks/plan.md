# Implementation Plan: Spark 0.2 - gaps, coherence and performance

The 0.1.0 roadmap (phases 1-14, tasks T1-T81, decisions A1-A8) and the networking
stream (phases 15-20, tasks T82-T102, decisions A9-A15) are complete. Their record
survives in git history, `CHANGELOG.md`, `docs/intent/spark-completion.md` and
`docs/intent/networking.md`. Phase and task numbering continue from them (phases 21+,
T103+), and everything below is the active work.

The agreed intent for this stream - audit findings, decisions and work order - is
recorded in [`docs/intent/spark-0.2.md`](../docs/intent/spark-0.2.md), the same way
Q11-Q16 live in `docs/intent/networking.md`.

### Carry-over from the previous streams (closed here)

Rewriting the plan and the task list would have dropped their few open items, so each
one is resolved explicitly (T107):

- **T97 (WebSocket server, optional)** is carried forward as **T113**, blocked on the
  `bytes` type (A16). Its acceptance criteria (a Spark server echoing a Spark client,
  including ping/pong and close; fixture plus suite) move with it unchanged.
- **The Phase 15 benchmark note** is closed. The `fib(27)` delta tracked the number and
  length of registered natives - adding dummy natives to the pre-change build
  reproduced it - so it was heap-layout noise rather than a hot-path change. The
  baseline measured for this stream is in the Baseline section above.
- **The "review with the user before Phase 16" gate** is closed: the user reviewed the
  plan and phases 16-20 shipped (see `CHANGELOG.md` and the git history).

This stream comes from a code-and-runtime audit of the 0.1.0 tree (2026-10-31): every
claim below was verified against the built interpreter, not read off the docs. It
covers four kinds of work, in this order of priority:

1. **Coherence** - the reference contradicts the implementation in places, and some
   artifacts are not in English (`AGENTS.md`).
2. **Add** - a byte-buffer type, the codecs/date/crypto/stdlib surface, a complete
   regex, and a production-grade HTTP client.
3. **Fix** - language ergonomics (loop `else`, multiple assignment), object protocols
   missing from the OOP model, and error values without a trace.
4. **Optimize** - allocator and GC, superinstruction coverage for the global path,
   scheduler syscall overhead, coroutine memory, build flags.

## Definition of Done (every task)

- Build is warning-free: `make` with `-O3 -Wall -Wextra -Wpedantic` (CI adds `-Werror`).
- `python3 tests/run_tests.py` passes (238 tests at the start of this stream) and new
  behavior has new fixtures: `tests/cases` (exact `.out`) or `tests/errors`
  (`exit:`/`stderr:`).
- Sanitizer run is clean when the task touches the VM, GC, compiler or a new native:
  `make clean && make CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic" LDFLAGS="-fsanitize=address,undefined" && python3 tests/run_tests.py && make clean && make`.
- `python3 benchmarks/run.py` keeps the `fib(27)` minimum **below 5 ms** unless the
  task is explicitly about performance (then the task states its own target and is
  measured with `python3 benchmarks/run_all.py`).
- Documentation updated where behavior changes: `README.md`, `docs/language.md`,
  `docs/architecture.md`, plus `CHANGELOG.md` (a "Breaking changes" entry for any
  change listed in A19) and `AGENTS.md` when a convention or a dependency changes.
- All artifacts in English (`AGENTS.md` section 5), including benchmark and example
  sources.
- New natives/methods are added to the reference table in `docs/language.md` with
  signature, limits and error behavior, and to the `README.md` native table.

## Baseline (for regression checks)

Measured on the development machine (WSL2, shared) on 2026-10-31, before this stream:

- Tests: **238 of 238 passing** (`python3 tests/run_tests.py`), clean under ASan/UBSan.
- No warnings with `-O3 -Wall -Wextra -Wpedantic`.

| Benchmark | Min (ms) | Median (ms) | | Benchmark | Min (ms) | Median (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| fib(27) | 4.83 | 4.90 | | alloc (1M) | 74.87 | 75.99 |
| echo (2k RTT) | 440.29 | 471.85 | | strings (200k) | 51.44 | 55.01 |
| http (200 req) | 157.36 | 159.35 | | string_builder (200k) | 5.11 | 5.19 |
| int_loop (5M) | 96.81 | 98.25 | | maps (200k) | 134.61 | 136.79 |
| float_loop (3M) | 57.88 | 58.59 | | sort (100k) | 4.07 | 4.19 |
| closures (1M) | 24.10 | 24.58 | | sort_by (100k) | 24.55 | 24.94 |
| higher_order (300k) | 28.09 | 28.63 | | | | |
| arrays (1M) | 47.03 | 47.47 | | | | |

Notes that matter for the performance phase:

- `fib(27)` sits ~3% under the 5 ms gate, so the gate has almost no headroom left.
- Most benchmarks run at the **top level**, where variables are globals: they measure
  the path with **no** fused superinstructions (`int_loop` compiles the accumulator as
  `GET_GLOBAL`/`SET_GLOBAL`). T147 adds function-body variants.
- `echo` and `http` are syscall-bound (~220 us and ~790 us per operation on WSL2);
  `maps`/`alloc` are allocation-bound. Those two are where the wins are.

## Architecture Decisions

- **A16 - `bytes` is a first-class immutable byte-string object** (`OBJ_BYTES`),
  sibling of `string`, not a reinterpretation of it. Indexing yields an integer
  `0..255`, `len` counts bytes, equality is by content, `print` shows a printable
  preview plus an escape form. Rationale: `string` stays UTF-8 text with
  character-based positions, so no existing program changes meaning, and the binary
  boundary becomes explicit in the type system. This is the blocker named in
  `docs/architecture.md` for the WebSocket server (T97). The surface is an immutable
  `bytes` plus a `bytes_builder()` (Q18): mutability lives in the builder, so `bytes`
  stays safe as a map key and trivial for the collector to trace.
- **A17 - New natives live in domain modules, not in `vm.c`.** `vm.c` is already ~9,000
  lines; follow the `src/net.c` / `src/json.c` / `src/regex.c` pattern: `src/codec.c`
  (base64/hex/deflate), `src/time.c`, `src/crypto.c`, `src/bytes.c`. `vm.c` keeps the
  core language, the VM integration and the shared helpers (`compare_values`,
  `arithmetic_values`, `add_values`).
- **A18 - The performance gate stays `fib(27)` min < 5 ms, and gets a second axis.**
  Every performance task states its own target (for example "`int_loop` min -15%") and
  is measured with `run_all.py` before and after, never guessed. The gate does **not**
  move for the GC work (Q26): an optimization that cannot hold it does not land, so the
  scheduler, allocator and collector changes are all reversible by construction.
- **A19 - Breaking changes are allowed (pre-1.0) but never silent.** Each one is
  recorded in the `CHANGELOG.md` "Breaking changes" section, in `docs/language.md`, and
  in this file; the fixtures that depend on the old behavior are updated in the same
  commit.
- **A20 - GC work is incremental and revertable**: (1) object arena + free lists,
  (2) `gc_stats()` and env-var tuning so the collector can be observed, (3) generational
  collection with a nursery. NaN-boxing (T148) comes last and only if the measured win
  justifies the risk to the gate. Q20 is answered as "decide with the measurements in
  hand": T148 stays in the plan as an explicit go/no-go point after T147, not as
  committed work, and leaving it out is a valid outcome.
- **A21 - The HTTP client becomes coroutine-aware in place.** Instead of shipping a
  parallel `*_async` API, the existing helpers yield like every other socket operation,
  so a program cannot block the scheduler by accident. This removes the documented
  "the HTTP client is synchronous" limit and is a documented breaking change.
- **A22 - Dependency policy unchanged, and now settled (Q17).** OpenSSL stays the single
  approved exception. Deflate/gzip support vendors a minimal `inflate` in `src/codec.c`
  instead of adding zlib, so `AGENTS.md` needs no new exception and the size cap that
  bounds a decompression bomb lives in code we own.
- **A23 - Additive language features need no redesign; the semantic changes have been
  settled.** Loop `else`, `break value`, multiple assignment, `range(start, end, step)`,
  `sort(array, comparator)`, the missing object protocols, getters and `enum` are
  additive and land as specified. Of the changes that touch existing semantics: ordered
  maps land (Q22), module namespaces land (Q24), and non-public fields do not - `private`
  and `readonly` are out of 0.2 because enforcing them in a dynamic language costs a
  runtime check on every field access, and `defer` is out because `try`/`finally` already
  covers it (Q23).
- **A24 - Embedding ships as an experimental public header.** `src/spark.h`, a static
  library target and documented structs, explicitly marked not ABI-stable before 1.0.
- **A25 - Editor support means tree-sitter, not an LSP (Q25).** A grammar plus queries
  and editor notes land in T152; a language server is not planned for 0.2, and staying
  out of it is deliberate: the grammar also gives the formatter and the linter a second
  parser to check against, which an LSP would not.
- **A26 - Tests never touch the public internet** (A15 stays in force), and every new
  native that can block keeps a bounded default timeout.
- **A27 - Regex moves to Perl/JavaScript leftmost-first semantics (Q27).** The Pike VM
  (`src/regex.c`) currently reports the leftmost-**longest** match, which makes lazy
  quantifiers meaningless and disagrees with every other engine a user has met. Lazy
  `*?`/`+?`/`??` land, the tie-breaking rule changes, and the patterns whose result
  changes (alternation inside a group, such as `a|ab`) are recorded in the
  `CHANGELOG.md` "Breaking changes" section with the fixtures updated in the same commit.
- **A28 - Top-level variables become locals of the main function in files (Q21)**, so
  top-level loops get the fused superinstructions instead of `GET_GLOBAL`/`SET_GLOBAL`.
  The REPL keeps global semantics (its variables persist across lines) and any name an
  imported module can observe stays global; the module and REPL cases are fixtured before
  the change, not after.
- **A29 - 0.2 ships module namespaces, not packaging (Q24).** `import ... as` and
  `export` land in T154 because they unblock a real `lib/`; the manifest, the resolver and
  the lockfile (T155) move to their own stream, since they add git and network access to
  the surface the test suite has to cover.

## Task List

### Phase 21 - Coherence and debt (P0; T103-T107 change no behavior, T159 only
messages)
- [x] T103 Intent doc for the 0.2 stream (`docs/intent/spark-0.2.md`)
- [x] T104 Negative-indexing semantics corrected in the reference
- [x] T105 Operator table and undocumented syntax in the reference
- [x] T106 Spanish identifiers in benchmarks and artifacts
- [x] T107 Carry-over reconciliation: T97 deferral and the old checkpoints
- [x] T159 Error messages: report the index the user wrote, and drop the "a instance"
      article (found during the T104/T105 review; it is a code change, so it takes the
      full definition of done, sanitizers included)

### Checkpoint: Phase 21
- [x] No claim in `docs/language.md` contradicts the built interpreter (for the
      sections T104/T105 rewrote)
- [x] All artifacts in English; suite and benchmark gates unchanged (239/239,
      `fib(27)` min 4.84 ms)
- [x] Review with the user before starting the `bytes` work (approved 2026-10-31)
- [x] T159 verified after that review: 243/243 with five new or updated fixtures,
      sanitizers clean, `fib(27)` min 4.95 ms

### Phase 22 - `bytes` and binary I/O (P1, A16)
- [ ] T108 `bytes` core object type
- [ ] T109 VM integration for `bytes`
- [ ] T110 Encoding bridges and `to_json`
- [ ] T111 Binary file and socket I/O
- [ ] T112 Serialization, docs and limits for `bytes`
- [ ] T113 WebSocket server on `bytes` (carry-over T97)

### Checkpoint: Phase 22
- [ ] `bytes` round-trips a valid UTF-8 string unchanged
- [ ] A Spark program writes and reads a real binary file byte-exactly
- [ ] Suite + sanitizers green, `fib(27)` min unaffected

### Phase 23 - Codecs, text, time, crypto and regex (P1, A17)
- [ ] T114 `base64_encode`/`base64_decode`, `hex_encode`/`hex_decode`
- [ ] T115 Deflate/gzip decode (vendored inflate, Q17)
- [ ] T116 `format` for printf-style text
- [ ] T117 Date and time formatting and parsing
- [ ] T118 `sha256`, `hmac_sha256`, `rand_bytes` through OpenSSL
- [ ] T119 Regex: `{n,m}`, lazy quantifiers, flags and match offsets
- [ ] T120 Regex: capture groups
- [ ] T121 Regex: `regex_split` and group references in `regex_replace`
- [ ] T122 Math natives
- [ ] T123 String and collection conveniences

### Checkpoint: Phase 23
- [ ] Every native has a fixture and a reference-table row
- [ ] The tokenizer example in `docs/language.md` uses capture groups and offsets
- [ ] Suite + sanitizers green

### Phase 24 - HTTP and network hardening (P1, A21)
- [ ] T124 gzip through the HTTP client
- [ ] T125 Keep-alive and connection reuse
- [ ] T126 The HTTP client yields its coroutine
- [ ] T127 HTTP server: concurrency, keep-alive, bounded request bodies
- [ ] T128 WebSocket keepalive/reconnect library helper

### Checkpoint: Phase 24
- [ ] A Spark program fetches a real gzip-compressed endpoint offline
- [ ] One process serves N concurrent HTTP clients without busy-waiting
- [ ] `http`/`echo` benchmarks measured and reported (targets in T126/T127)

### Phase 25 - Language ergonomics and object protocols (P2, A23)
- [ ] T129 `else` on loops
- [ ] T130 `break value`
- [ ] T131 Multiple assignment and swap
- [ ] T132 `__iter__`, `__call__`, `__contains__`, `__bool__`
- [ ] T133 `__hash__` and instance map keys
- [ ] T134 Reflected operators
- [ ] T135 Error values: `trace` and a built-in `Error` hierarchy
- [ ] T136 `to_json` for instances and pretty printing
- [ ] T137 Insertion-ordered maps (decided: Q22)
- [ ] T138 Getters and `enum`

### Checkpoint: Phase 25
- [ ] Every new protocol/feature has positive and error fixtures
- [ ] `docs/language.md` sections 3-9 and 14 list every operator and protocol
- [ ] Suite + sanitizers + benchmark gate green

### Phase 26 - Performance (P2/P3, all measured, A18/A20)
- [ ] T139 Object allocator: arena and free lists
- [ ] T140 GC observability and tuning
- [ ] T141 Generational GC with a nursery
- [ ] T142 Superinstructions for the global path and the loop idiom
- [ ] T143 Script scoping for top-level variables (files only, Q21)
- [ ] T144 Scheduler: remove the extra `poll(2)` per turn
- [ ] T145 Cheap coroutines: smaller initial stacks
- [ ] T146 Build: LTO and PGO targets
- [ ] T147 Benchmark suite: function-body variants and a recorded baseline
- [ ] T148 NaN-boxing of `Value` (Q20: go/no-go after T147)

### Checkpoint: Phase 26
- [ ] `fib(27)` min < 5 ms with headroom restored (target: < 4.5 ms)
- [ ] `alloc`, `maps`, `arrays` measured against the baseline table above
- [ ] Every optimization backed by before/after numbers in the task and `CHANGELOG.md`

### Phase 27 - Tooling and ecosystem (P3, A24/A25)
- [ ] T149 CLI: `--check`, `-e`, `--time`, `--gc-stats`, `--`
- [ ] T150 REPL: line editing and history
- [ ] T151 Linter: undefined globals and unused imports
- [ ] T152 tree-sitter grammar, queries and editor files (no LSP in 0.2)
- [ ] T153 Public embedding header and library build
- [ ] T154 Module namespaces and `import ... as` (decided: Q24)
- [ ] T156 Spark standard library modules in `lib/`
- [ ] T157 Parser/lexer fuzz targets and a CI job
- [ ] T158 Coverage threshold and multi-platform release artifacts

### Deferred to a later stream (decided, not 0.2)
- [ ] T155 Project manifest and minimal package manager (deferred by Q24/A29; the task
      detail stays in `tasks/todo.md` so the intent is not lost)

### Checkpoint: Complete
- [ ] All acceptance criteria met; suite, sanitizers and benchmark gates green
- [ ] `docs/language.md`, `docs/architecture.md`, `README.md`, `AGENTS.md`,
      `CHANGELOG.md` and `docs/intent/spark-0.2.md` match the code
- [ ] Ready for review

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| The GC rewrite (T139-T141) regresses the 5 ms gate or destabilizes coroutines | High | Land the allocator and `gc_stats` first with no semantic change; keep the collector behind the existing `mem_*` seam so it can be reverted; measure every step with `run_all.py`; the gate does not move (Q26), so a change that cannot hold it comes out |
| NaN-boxing (T148) touches every hot path | High | Go/no-go after T147 with the measurements in hand (Q20); a spike measures the win on `fib`/`int_loop` before the full change; never lands in the same commit as another optimization; dropping it is a valid outcome |
| Script scoping (T143) changes global visibility semantics | High | Fixed scope (Q21/A28): files only, REPL and module-visible names stay global; the module and REPL cases are fixtured before the change |
| The coroutine-aware HTTP client (T126) destabilizes the scheduler, as phase 16 once did | High | Open with a spike on one request path, keep the blocking implementation behind the same entry points, run the fd-leak and 32-client fixtures, keep TLS out of the first slice |
| A second dependency (zlib) contradicts the "no dependencies" rule | Medium | Decision Q17; prefer a vendored inflate (~1k lines, no policy change) unless the user prefers zlib; document either way in `AGENTS.md` and `CHANGELOG.md` |
| Breaking changes (A19) silently invalidate examples, docs or fixtures | Medium | Each breaking task updates `CHANGELOG.md`, the reference, examples and fixtures in the same commit; the suite is the guard |
| 56 tasks is more than one release; half-finished phases accumulate | Medium | Phases are independently shippable; the P0/P1/P2/P3 labels in the headings say what can be cut; each phase ends at a green checkpoint |
| Perf work on a shared machine produces noisy numbers | Medium | Use `run_all.py` min-of-7, report min and median, and only act on deltas above a few percent |
| New natives widen the attack surface (decompress, crypto, binary I/O) | Medium | Every native has a size cap and a categorized error, with a fixture that trips it (A26, T99's checklist) |
| `bytes` changes `len()`/indexing expectations across the stdlib | Medium | A16 keeps `string` semantics untouched; the two types are never implicitly converted; mixed operations are a `TypeError` with a message that names `to_bytes`/`to_string` |

## Open Questions

All answered on 2026-10-31. They are recorded here and must be copied into
`docs/intent/spark-0.2.md` by T103, the way Q11-Q16 were recorded in
`docs/intent/networking.md`.

- **Q17 (deflate dependency) - DECIDED:** vendor a minimal `inflate` in `src/codec.c`;
  no second dependency, no change to the `AGENTS.md` dependency rule (A22).
- **Q18 (`bytes` API shape) - DECIDED:** immutable `bytes` plus `bytes_builder()`; no
  mutable `bytearray` in 0.2 (A16).
- **Q19 (HTTP async) - DECIDED:** the existing helpers become coroutine-aware in place,
  a documented breaking change; no parallel `*_async` API (A21).
- **Q20 (NaN-boxing) - DECIDED:** decide after T139-T147 with the measurements in hand;
  T148 is a go/no-go point and dropping it is a valid outcome (A20).
- **Q21 (script scoping) - DECIDED:** yes for files, no for the REPL; module-visible
  names stay global (A28).
- **Q22 (ordered maps) - DECIDED:** `map` adopts insertion order, keeping the hash table
  for lookup; iteration order becomes specified behavior and is documented (T137).
- **Q23 (visibility and enums) - DECIDED:** getters and `enum` are in 0.2;
  `private`/`readonly` are out (a runtime check per field access for a guarantee the
  linter can give for free) and `defer` is out (`try`/`finally` already covers it).
- **Q24 (modules and packaging) - DECIDED:** module namespaces and `import ... as` land
  in 0.2 (T154); the manifest, resolver and lockfile move to a later stream (A29), so
  T155 is deferred.
- **Q25 (editor support) - DECIDED:** tree-sitter grammar, queries and editor notes; no
  LSP in 0.2 (A25).
- **Q26 (benchmark gate during the GC work) - DECIDED:** the gate stays `fib(27)` min
  < 5 ms with no exceptions; the collector work must hold it or come out (A18).
- **Q27 (regex semantics) - DECIDED:** move to Perl/JavaScript leftmost-first semantics
  and implement lazy quantifiers, as a documented breaking change (A27).

## Parallelization

- **Independent:** T104-T107 (docs), T114/T116/T118 (codecs, time, crypto - one file
  each per A17), T122/T123 (math, string conveniences), T149/T150 (CLI, REPL),
  T152/T157 (tree-sitter, fuzzing).
- **Sequential:** T108 -> T109 -> T110 -> T112 -> T113 (the `bytes` chain, and T113 also
  needs T119-T121); T114 -> T115 -> T124 (codec -> deflate -> HTTP gzip); T125 -> T126
  (keep-alive before the scheduler rewrite); T139 -> T140 -> T141 (allocator before GC);
  T119 -> T120 -> T121 (regex offsets before groups before split/replace).
- **Coordinate first:** the `bytes` API surface (T108) must freeze before T110-T113,
  T115 and T124, exactly as the socket API had to freeze before phase 17.
- **Not parallelizable with anything:** T141, T143, T148 (GC, scoping, value layout) -
  each one touches shared state and needs a quiet measurement window.

## Out of Scope (this stream)

- JIT/AOT native code generation (T50 stays deferred) and a register-based VM.
- OS threads on top of the coroutines: the model stays cooperative and single-threaded.
- HTTP/2, HTTP/3/QUIC, proxies, a cookie jar and streaming request bodies.
- WebSocket `permessage-deflate` and server-side sharding/rate-limit bookkeeping.
- Windows-native sockets and TLS: WSL stays the supported Windows path.
- A non-blocking DNS resolver (resolution keeps using `getaddrinfo`).
- Deleting `exec` or sandboxing the runtime: Spark is still not a security boundary.
