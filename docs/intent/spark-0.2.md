# Intent: Spark 0.2 - gaps, coherence and performance

Confirmed with the user on 2026-10-31. This records the audit findings, the agreed
scope, the decisions and the work order for the 0.2 work stream. It is the input for
phases 21-27 in `tasks/plan.md` and `tasks/todo.md`; update it if the intent changes.

## Outcome

Spark 0.2 closes the distance between what the reference promises and what the
interpreter does, adds the binary and standard-library surface a program needs to talk
to real services (`bytes`, codecs, dates, crypto, a complete regex, an HTTP client that
does not block the scheduler), fills the ergonomic holes in the language, and makes the
allocation-heavy benchmarks faster without giving up the `fib(27)` gate.

## Why now (audit findings)

The 0.1.0 tree was audited on 2026-10-31 by executing the interpreter, not by reading
the docs. Findings, by area:

- **Coherence.** `docs/language.md` says negative indexes are runtime errors; the
  interpreter counts them from the end (a decision recorded in
  `docs/intent/spark-completion.md` that never reached the reference). The section 3
  precedence table omits `**`, `?.`, `??=`, `++`/`--`, slicing, `in` and `instanceof`,
  and the reference never mentions slice syntax `a[start:end:step]` or `fn(x) => expr`
  at all. Eleven benchmark sources use Spanish identifiers, which the project's
  English-artifacts convention forbids. The previous stream was closed with one task
  (T97, WebSocket server) and two checkpoint notes still open.
- **Binary data.** There is no byte-buffer type. `string` is UTF-8 text with
  character-based positions, so binary payloads have to travel as strings and a
  non-UTF-8 byte cannot be represented. This is the blocker `docs/architecture.md`
  names for the WebSocket *server*: frames carry binary opcodes.
- **Standard library.** No `base64`, `hex`, `gzip`, `sha256`, `hmac_sha256`,
  `rand_bytes`, `format`, `strftime`, no `seek`/`tell` on files, no
  `range(start, end, step)` and no `sort(array, comparator)`. The regex engine has no
  `{n,m}`, no lazy quantifiers, no flags, no capture groups, no offsets, no
  `regex_split` and no group references in `regex_replace` - and it reports the
  leftmost-**longest** match, which no other engine does.
- **HTTP.** The client is documented as synchronous (a limit named in the phase 16
  record), sends no `Accept-Encoding`, cannot decompress a response and opens one
  connection per request. Every one of those is visible to a Spark program.
- **Language ergonomics.** No `else` on loops, no `break value`, no multiple
  assignment, no `__iter__`/`__call__`/`__contains__`/`__bool__`/`__hash__`, no
  reflected operators, no `trace` on an error value and no built-in error hierarchy, no
  `to_json` for instances, unspecified map iteration order, no getters and no `enum`.
- **Performance.** `fib(27)` sits about 3% under the 5 ms gate, so it has almost no
  headroom. The collector is mark-sweep with an adaptive threshold: no generations, no
  observability. Every benchmark runs at the top level, where variables are globals and
  the fused superinstructions do not apply. The scheduler issues an extra `poll(2)` per
  turn, coroutines start with 2048-slot stacks, and the build has no LTO or PGO.
- **Tooling.** No `--check`, `-e`, `--time` or `--gc-stats`; the REPL is `getline` with
  no history or line editing; no linter rule for undefined globals; no editor support;
  no embedding header; no module namespaces; no `lib/`; no fuzzing.

## Success criteria

- Warning-free build, full suite green (238 tests at the start of the stream, every new
  behavior fixtured), sanitizers clean for VM/GC/compiler/native changes.
- `fib(27)` minimum stays below 5 ms after every task; performance tasks state their own
  target and are measured with `benchmarks/run_all.py` before and after.
- Tests never touch the public internet (local servers, ephemeral ports, bounded
  timeouts); the certificates in `tests/tls/` remain throwaway fixtures.
- Every breaking change is recorded in `CHANGELOG.md`, `docs/language.md` and
  `tasks/plan.md`, with the fixtures that depend on the old behavior updated in the same
  commit.
- `README.md`, `docs/language.md`, `docs/architecture.md`, `AGENTS.md` and
  `CHANGELOG.md` match the code.
- All artifacts in English.

## Decisions (A16-A29)

Recorded in full in `tasks/plan.md`; the one-line rationale:

- **A16** `bytes` is a first-class immutable byte-string object, sibling of `string`,
  with mutability confined to a `bytes_builder()`. `string` keeps character positions,
  so no existing program changes meaning (Q18).
- **A17** New natives live in domain modules (`src/codec.c`, `src/time.c`,
  `src/crypto.c`, `src/bytes.c`), following the `src/net.c` pattern; `vm.c` keeps the
  core language and the shared helpers.
- **A18** The performance gate stays `fib(27)` min < 5 ms and does not move for the GC
  work; every performance task states its own target and is measured, never guessed
  (Q26).
- **A19** Breaking changes are allowed before 1.0 but never silent.
- **A20** GC work is incremental and revertable (arena/free lists, then `gc_stats`,
  then a generational nursery); NaN-boxing comes last and only if the measurements
  justify the risk to the gate (Q20).
- **A21** The HTTP client becomes coroutine-aware in place instead of growing a second
  `*_async` API (Q19).
- **A22** OpenSSL stays the single approved dependency; deflate vendors a minimal
  `inflate` in `src/codec.c` rather than adding zlib (Q17).
- **A23** The additive language features land as specified; of the semantic changes,
  ordered maps and namespaces land, while `private`, `readonly` and `defer` do not
  (Q22, Q23, Q24).
- **A24** Embedding ships as an experimental `src/spark.h` plus a static library, not
  ABI-stable before 1.0.
- **A25** Editor support means a tree-sitter grammar, queries and editor notes; an LSP
  is not planned for 0.2 (Q25).
- **A26** Tests never touch the public internet, and every native that can block keeps
  a bounded default timeout.
- **A27** Regex moves to Perl/JavaScript leftmost-first semantics so lazy quantifiers
  mean something, as a documented breaking change (Q27).
- **A28** Top-level variables become locals of `main` in files, so top-level loops get
  the fused superinstructions; the REPL keeps global semantics and module-visible names
  stay global (Q21).
- **A29** 0.2 ships module namespaces and `import ... as`, not a package manager; the
  manifest, resolver and lockfile move to a later stream.

## Answers to the open questions (Q17-Q27)

All answered by the user on 2026-10-31:

| Question | Answer |
| --- | --- |
| Q17 deflate dependency | Vendor a minimal `inflate`; no second dependency. |
| Q18 `bytes` API shape | Immutable `bytes` plus `bytes_builder()`; no mutable `bytearray` in 0.2. |
| Q19 HTTP async | The existing helpers yield their coroutine in place; documented breaking change. |
| Q20 NaN-boxing | Decide after T139-T147 with the measurements in hand; T148 is a go/no-go point and dropping it is a valid outcome. |
| Q21 script scoping | Yes for files, no for the REPL; module-visible names stay global. |
| Q22 ordered maps | `map` adopts insertion order and keeps the hash table for lookup; iteration order becomes specified behavior. |
| Q23 visibility and enums | Getters and `enum` are in 0.2; `private`, `readonly` and `defer` are out. |
| Q24 modules and packaging | Module namespaces and `import ... as` land in 0.2; the manifest, resolver and lockfile are deferred. |
| Q25 editor support | tree-sitter grammar, queries and editor notes; no LSP in 0.2. |
| Q26 benchmark gate | The gate stays `fib(27)` min < 5 ms with no exceptions; work that cannot hold it comes out. |
| Q27 regex semantics | Move to Perl/JavaScript leftmost-first semantics and implement lazy quantifiers. |

## Work order

1. **Coherence (T103-T107),** no behavior change: the intent doc, the reference
   corrections, the English artifacts and the carry-over reconciliation. Cheapest phase,
   and it leaves the tree consistent before new types arrive.
2. **`bytes` (T108-T113):** the type, VM integration, encoding bridges, binary file and
   socket I/O, serialization and limits, then the WebSocket server that T97 was blocked
   on. The `bytes` surface freezes at T108 because T110-T113, T115 and T124 all consume
   it.
3. **Codecs, text, time, crypto and regex (T114-T123):** one module per domain, each
   native with a fixture and a reference row. Regex is the long pole: offsets before
   capture groups before split and replacements.
4. **HTTP and network hardening (T124-T128):** gzip through the client, keep-alive,
   then the scheduler-facing change (the client yields), then the server, then the
   WebSocket helper. The scheduler change keeps a spike, a fallback and the fd-leak and
   32-client fixtures.
5. **Language ergonomics and object protocols (T129-T138):** loop `else`, `break value`,
   multiple assignment, the object protocols, reflected operators, error traces and the
   `Error` hierarchy, instance JSON, ordered maps, getters and `enum`.
6. **Performance (T139-T148),** all measured: allocator, GC observability, generational
   GC, superinstructions for the global path, script scoping, scheduler `poll`,
   coroutine memory, LTO/PGO, the benchmark suite, and the NaN-boxing go/no-go.
7. **Tooling and ecosystem (T149-T158):** CLI flags, REPL line editing, the linter,
   tree-sitter, the embedding header, module namespaces, `lib/`, fuzzing, coverage and
   release artifacts.

## Deliberately out of scope

Decided, not forgotten:

- `private`, `readonly` and `defer` (A23): field-access enforcement costs a runtime
  check on every access for a guarantee the linter can give for free, and
  `try`/`finally` already covers `defer`.
- A project manifest and package manager (A29, T155): it adds git and network access to
  the surface the test suite has to cover.
- A language server (A25); the tree-sitter grammar comes first and also gives the
  formatter and the linter a second parser to check against.
- JIT/AOT code generation and a register-based VM; OS threads (the model stays
  cooperative and single-threaded); HTTP/2, HTTP/3/QUIC, proxies, cookie jars and
  streaming request bodies; WebSocket `permessage-deflate`; Windows-native sockets and
  TLS; a non-blocking DNS resolver; sandboxing the runtime.
