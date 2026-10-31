# Spark task list

Detailed, dependency-ordered task list for the roadmap in `tasks/plan.md`. Execute top
to bottom; do not start a phase before its checkpoint is approved. Every task must
satisfy the Definition of Done in the plan (warning-free build, tests, docs,
sanitizers for VM/native tasks, benchmark gate, English artifacts).

Legend: **scope** S (1-2 files), M (3-5), L (5-8). Anything XL is split further.

Baseline at the start of this stream (2026-10-31): 238 of 238 tests passing, `fib(27)`
min 4.83 ms / median 4.90 ms, no warnings. The full benchmark baseline table is in
`tasks/plan.md`.

---

## Phase 21 - Coherence and debt (P0)

### T103 - Intent doc for the 0.2 stream
**Description:** Record the outcome, decisions and work order of this stream in
`docs/intent/spark-0.2.md`, the way `docs/intent/spark-completion.md` and
`docs/intent/networking.md` record theirs, so a future session can see why this order
was chosen.
**Acceptance criteria:**
- [x] The doc lists the audit findings that motivated each phase, the decisions A16-A29
      and the answers to Q17-Q27 (all decided on 2026-10-31), including what was
      deliberately left out (`private`/`readonly`, `defer`, a package manager, an LSP).
- [x] It is linked from `AGENTS.md` section 7 and from `tasks/plan.md`.
**Verification:**
- [x] Read-back check: every phase in `tasks/plan.md` appears in the work order.
**Dependencies:** None. **Files:** `docs/intent/spark-0.2.md`, `AGENTS.md`, `tasks/plan.md`.
**Scope:** S.

### T104 - Negative-indexing semantics corrected in the reference
**Description:** The reference contradicts the interpreter, which follows the
unification decision in `docs/intent/spark-completion.md`: negative positions count
from the end for simple indexing, `slice` and `substr`. Fix section 7 ("Indexes must be
non-negative integers within range"), section 9 ("A negative, non-integer or
out-of-range index is a runtime error", "Negative `start`/`count` are errors") and the
`slice`/`substr` rows in the section 10 table.
**Acceptance criteria:**
- [x] Every corrected sentence matches a verified run: `[1,2,3][-1]` is `3`,
      `"spark"[-1]` is `k`, `slice([1,2,3],-2)` is `[2,3]`, `substr("abcdef",-2,1)` is `e`.
- [x] The difference between indexing (out of range is an error) and slicing (clamped)
      is stated in one place and cross-referenced from both sections.
- [x] `tests/cases/negative_indexing.spark` pins the behaviors; the out-of-range error
      case lives in `tests/errors/negative_index_out_of_range.spark`, because
      `tests/cases` only holds programs that succeed.
**Verification:**
- [x] Each documented example executed with `./spark`; suite green.
**Dependencies:** None. **Files:** `docs/language.md`, `tests/cases/`, `tests/errors/`.
**Scope:** S.

**Done notes:** the canonical statement sits in section 3 ("Indexing and slicing"),
with sections 7 and 9 pointing at it. Three further contradictions turned up and were
fixed here: the section 9 example indexed `"spark"[5]` (out of range) and printed the
wrong characters; `substr` rejects a negative `count` but accepts a negative `start`;
and the `slice` native has no `step` parameter and clamps negative positions. One wart
was left alone as it is code, not docs: an out-of-range negative index reports the
normalized position (`a[-4]` on three elements says `-1`).

### T105 - Operator table and undocumented syntax in the reference
**Description:** Section 3's precedence table omits operators the interpreter
implements. Add `**`, `?.` (optional chaining, including the method form), `??=`,
`++`/`--` (prefix and postfix), slicing `a[start:end:step]`, array spread, `in`
(membership) and `instanceof`, with precedence and associativity for each. Document
`fn(x) => expr` (short-form lambdas, already fixtured in `tests/cases/lambdas.spark` but
absent from the reference) and check the statement "assignments are statements" still
holds for the compound operators.
**Acceptance criteria:**
- [x] The table is complete against the lexer's operator tokens
      (`src/lexer.h` TOKEN_* list) and each row has a running example in the docs.
- [x] Precedence claims are verified by executing grouped and ungrouped forms
      (`print(2 + 3 & 1)` style checks).
- [x] A short "undocumented until now" note is not needed in the final text: the
      reference simply describes the language.
**Verification:**
- [x] One probe program per row executed; suite green.
**Dependencies:** None. **Files:** `docs/language.md`.
**Scope:** S.

**Done notes:** the verified precedence order inserts `in`/`instanceof` **between** the
comparisons and `==` (`1 < 2 in [true]` is `(1 < 2) in [true]`; `1 & 1 in [1]` fails
because `in` binds tighter than `&`), and puts `**` above `*` with unary minus binding
tighter than `**` (`-2 ** 2` is `4`, unlike Python). `++`/`--` are expressions, unlike
`=` and the compound assignments, which stay statements - that qualification is now in
sections 3 and 4. `a?.b` guards only its own link, slicing has its own subsection with
the clamp-versus-error rule, and the `is_a`/`isinstance` rows were corrected (`is_a`
rejects type-name strings).

### T106 - Spanish identifiers in benchmarks and artifacts
**Description:** The project keeps every artifact in English, but 11 files under
`benchmarks/` used Spanish identifiers (`inicio`, `fin`, `resultado`) and the audit found
the same problem well beyond them: 46 fixture pairs had Spanish file names in
`tests/cases` and `tests/errors`, 13 fixture sources had Spanish identifiers, comments,
messages or data inside, and `tests/run_tests.py` carried a Spanish function name in its
REPL session and referenced three renamed fixtures. Rename all of it. The premise was
also fixed: the English rule lived only in `README.md`, so it is now stated in
`AGENTS.md` section 5, where the plan's definition of done points.
**Acceptance criteria:**
- [x] No Spanish identifier remains in `benchmarks/`, `examples/`, `lib/` or `tests/`.
- [x] Benchmark outputs are unchanged (the `.out` expectations and the printed numbers
      keep their meaning).
- [x] `grep -rn` for the known names returns nothing.
**Verification:**
- [x] `python3 tests/run_tests.py` green; `python3 benchmarks/run_all.py` runs.
**Dependencies:** None. **Files:** `benchmarks/*.spark`, `examples/`, `lib/`, `tests/`,
`AGENTS.md`.
**Scope:** L (the original estimate of S was wrong; the inventory is in the done notes).

**Done notes:** 46 fixture pairs renamed with `git mv` (`cases/`: aritmetica, bucles,
condicionales, fase4_e2e, funciones, hola, iteracion, lenguaje_completo, mapas, memoria,
nativas, textos; `errors/`: argumentos, asignacion_invalida, asignar_texto, assert_falla,
bitnot_no_entero, bitwise_no_entero, break_fuera, contains_tipo, div_cero,
div_no_entero, division_cero, falta_punto_y_coma, get_no_mapa, has_no_mapa,
indice_fuera, indice_texto, insert_fuera, iterar_no_iterable, llamada_a_no_funcion,
llave_sin_cerrar, randint_invalido, reduce_vacio, regex_invalido, remove_fuera,
repeat_negativo, return_fuera, shift_fuera, slice_negativo, sort_mixto, sqrt_negativo,
texto_sin_cerrar, tipos, variable_en_inicializacion, variable_indefinida). Three fixtures
were translated whole (`full_language`, `maps`, `closures`) because their comments,
assert messages and data were Spanish; `run_tests.py` now points at `hello.spark`,
`type_mismatch.spark` and `missing_semicolon.spark`. The `ñandú` specimens stay: they
are UTF-8 test data, not prose. Suite 238/238 and `fib(27)` min 4.92 ms after the
change; `run_all.py` unchanged.

### T107 - Carry-over reconciliation: T97 deferral and the old checkpoints
**Description:** The previous plan and task list were left with unchecked items. Record
their resolution explicitly instead of letting it disappear with the rewrite: T97
(WebSocket server) moves to T113, blocked on the `bytes` type; the Phase 15 benchmark
note (the `fib(27)` delta was heap-layout noise from registering natives, not a hot-path
change) and the "review with the user" gates are closed here with a pointer to the
baseline table in `tasks/plan.md`.
**Acceptance criteria:**
- [x] No open item from `git show HEAD:tasks/plan.md` or `HEAD:tasks/todo.md` is lost:
      each is either carried forward as a task or explicitly closed with a reason.
- [x] `docs/intent/networking.md` gets a one-line status note that its stream is closed
      and where T97 continues.
**Verification:**
- [x] Diff review of the old files against the new ones as a checklist.
**Dependencies:** None. **Files:** `tasks/plan.md`, `tasks/todo.md`, `docs/intent/networking.md`.
**Scope:** S.

**Done notes:** the old files held four unchecked items, all accounted for. T97 (the
WebSocket server) and its two acceptance lines move to T113 with the criteria unchanged;
the Phase 15 benchmark note and the "review with the user before Phase 16" gate are
closed in a new "Carry-over from the previous streams" section in `tasks/plan.md`, with
the noise explanation and the measured baseline. `docs/intent/networking.md` now opens
with the closed-stream status line.

### T159 - Error messages: the index the user wrote, and article agreement
**Description:** Two user-facing messages report the wrong thing. (1) An out-of-range
negative index reports the normalized position, because every index path adjusts
`position` in place before checking it: `[1,2,3][-4]` says `index out of range: -1
(size 3)`, naming a value the program never wrote. Keep the raw index for the message
and normalize only for the lookup. (2) `"can only update an array or a map element,
tried to update a %s"` produces `tried to update a instance`; use a phrasing that needs
no article, for example `received %s`, which is already the style of the other natives.
**Acceptance criteria:**
- [ ] `[1,2,3][-4]` reports `index out of range: -4 (size 3)`; positive out-of-range
      indexes are unchanged (`index_out_of_range` still says `5`).
- [ ] No message in `src/` produces `a instance` or another article mismatch for the
      values `value_type_name` can return.
- [ ] `tests/errors/negative_index_out_of_range.spark` and its `.out` are updated in the
      same commit, plus a fixture for the update message.
**Verification:**
- [ ] `python3 tests/run_tests.py` green; sanitizer run clean (VM change);
      `python3 benchmarks/run.py` still below 5 ms.
**Dependencies:** None (found in the T104/T105 review). **Files:** `src/vm.c`,
`tests/errors/`.
**Scope:** S.

## Checkpoint: Phase 21
- [x] No claim in `docs/language.md` contradicts the built interpreter (verified for
      every section T104/T105 rewrote, plus two contradictions found on the way: the
      `"spark"[5]` example and the `is_a` row)
- [x] All artifacts in English; suite and benchmark gates unchanged (239/239,
      `fib(27)` min 4.84 ms)
- [x] Review with the user before starting the `bytes` work (approved 2026-10-31)

---

## Phase 22 - `bytes` and binary I/O (P1, decision A16)

### T108 - `bytes` core object type
**Description:** Add `OBJ_BYTES` as an immutable byte sequence, sibling of `ObjString`:
`unsigned char *data`, `int length`, content equality, and a GC path. Natives:
`bytes(n)` (n zero bytes), `bytes_from(array_of_ints)`, `bytes_from(string)` (raw bytes,
no encoding conversion), plus `len(bytes)`, `type(bytes)` reporting `"bytes"`. New file
`src/bytes.c` / `src/bytes.h` registering through `bytes_init()` from `vm_init`, the way
`net_init()` does (`src/vm.c:8489`); `Makefile` already globs `src/*.c`.
**Acceptance criteria:**
- [ ] `bytes` is immutable: no native or operator mutates it in place.
- [ ] `values_equal` compares content; two identical `bytes` are `==` and usable as the
      same map key.
- [ ] `type(b)` is `"bytes"`, `print(b)` shows a printable preview plus an escape form,
      and `str(b)` is documented as the preview (not a lossless conversion).
- [ ] Passing a `string` where `bytes` is required is a `TypeError` naming
      `bytes_from`/`to_string`; there is no implicit conversion (A16).
**Verification:**
- [ ] `tests/cases/bytes_basics.spark`; error fixtures for the wrong-type and
      negative-size cases; suite + ASan/UBSan.
**Dependencies:** None. **Files:** `src/value.h`, `src/object.h`, `src/object.c`,
`src/bytes.c`, `src/bytes.h`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T109 - VM integration for `bytes`
**Description:** Wire `bytes` into the operations the VM already routes per type:
integer indexing (`b[0]` -> `0..255`), negative indexing, `[:]` slicing, iteration
(`for (var byte in b)` yields integers, no allocation), `contains`, `+` for
concatenation (bytes + bytes only), `bytes_builder()` mirroring `string_builder()` if
Q18 says yes. Reuse `index_to_position` (`src/vm.c:5062`) rather than duplicating index
validation.
**Acceptance criteria:**
- [ ] Indexing out of range is the existing `IndexError`; slicing clamps, consistent
      with arrays and per T104.
- [ ] `for (var byte in b)` does not allocate per element (unlike the string path at
      `src/vm.c:6559`, which materializes an array of one-character strings).
- [ ] Mixed `bytes + string` is a `TypeError` with the A16 message.
**Verification:**
- [ ] Fixtures for indexing, slicing, iteration, concatenation and each error; suite +
      ASan/UBSan; `python3 benchmarks/run.py` unchanged.
**Dependencies:** T108. **Files:** `src/vm.c`, `src/bytes.c`, `src/object.c`, `tests/`,
`docs/language.md`. **Scope:** M.

### T110 - Encoding bridges and `to_json`
**Description:** Explicit conversions: `to_string(bytes)` (validates UTF-8 and raises
`ValueError` with the offending offset when invalid), `to_bytes(string)`,
`bytes_to_array(bytes)`, and JSON integration (`to_json(bytes)` emits a base64 string
with a documented marker, `from_json` does not produce bytes).
**Acceptance criteria:**
- [ ] A `string.to_bytes().to_string()` round trip is byte-identical for ASCII, Latin-1
      range codepoints, 4-byte codepoints and a 1 MB random payload.
- [ ] Invalid UTF-8 in `to_string` raises `ValueError`; the message names the offset and
      does not echo the payload.
- [ ] `bytes` in `to_json` is documented and `from_json(to_json(b))` returns the
      original bytes when the documented decoding helper is applied.
**Verification:**
- [ ] Fixtures including the 1 MB round trip; suite + sanitizers.
**Dependencies:** T108, T109. **Files:** `src/bytes.c`, `src/json.c`, `tests/`,
`docs/language.md`. **Scope:** S.

### T111 - Binary file and socket I/O
**Description:** Let the file and socket layers move bytes without a UTF-8 detour:
`read_bytes(path, max?)`, `write_bytes(path, bytes)`, `append_bytes`, `read(file, n)`
returning `bytes` in binary mode (`open(path, "rb"/"wb")`), and `send`/`recv` accepting
and returning `bytes` on a socket opened for binary use. Keep the existing string forms
unchanged for compatibility.
**Acceptance criteria:**
- [ ] A real binary file (for example `/bin/ls`, 9.6 MB) read with `read_bytes` and
      written back with `write_bytes` is byte-identical (`cmp`), with no NUL truncation
      and no `len()` character reinterpretation.
- [ ] Binary `recv`/`send` round-trips arbitrary bytes over a local socket, including
      `0x00`, `0xFF` and a 1 MB payload.
- [ ] Size caps are enforced with a categorized error (`read` accepts at most 16 MiB,
      matching the existing limit table).
**Verification:**
- [ ] Fixtures with a local server; suite + ASan/UBSan; fd-count assertion kept.
**Dependencies:** T108-T110. **Files:** `src/bytes.c`, `src/vm.c` (file natives),
`src/net.c`, `tests/`, `docs/language.md`. **Scope:** M.

### T112 - Serialization, docs and limits for `bytes`
**Description:** Decide and implement what happens to a `bytes` constant in `.sparkc`
bytecode (serialize as a length-prefixed blob, or reject with a clear compile error),
then document the type: a section in `docs/language.md`, the limits it obeys, the
`type()`/`print` behavior, the conversion rules and the new native rows in the section
10 and README tables.
**Acceptance criteria:**
- [ ] `--compile` then run of a program with a `bytes` literal works, or fails with the
      documented error; either way the format version is bumped if it changes
      (`src/bytecode.c`).
- [ ] `docs/architecture.md` describes the object model entry and the GC path.
- [ ] The `README.md` native table and "Known limits" reflect the new type.
**Verification:**
- [ ] Round-trip fixture for the `.sparkc` path; suite; docs cross-check against the
      registered native list.
**Dependencies:** T108-T111. **Files:** `src/bytecode.c`, `docs/language.md`,
`docs/architecture.md`, `README.md`, `tests/`. **Scope:** S.

### T113 - WebSocket server on `bytes` (carry-over T97)
**Description:** The WebSocket server was deferred because a pure-Spark frame writer
cannot emit arbitrary length and mask bytes without a byte buffer
(`docs/architecture.md:545`). With `bytes` in place, implement the server: accept the
upgrade on the `lib/http.spark` server, then speak the same framing as the client
(text/binary/ping/pong/close, fragmentation, masking validation). Decide native
(`src/net.c`, sharing the client's framing helpers) versus Spark library
(`lib/ws.spark`); the native route avoids duplicating the framing rules, the library
route keeps the native surface small (Q13's reasoning).
**Acceptance criteria:**
- [ ] A Spark server and a Spark client complete the handshake and exchange text,
      binary, ping/pong and a clean close.
- [ ] Masked client frames are accepted; unmasked client frames are rejected with
      `1002`, mirroring the client's validation.
- [ ] Limits match the client (16 MiB frame/message, 125-byte control payload,
      123-byte close reason) with a fixture per limit.
**Verification:**
- [ ] Fixtures: Spark server + Spark client, plus the existing Python client; suite +
      ASan/UBSan; example updated and documented.
**Dependencies:** T108-T110, T119-T121 (for a Spark-side test harness). **Files:**
`src/net.c` or `lib/ws.spark`, `examples/`, `tests/`, `docs/language.md`,
`docs/architecture.md`. **Scope:** M.

## Checkpoint: Phase 22
- [ ] `bytes` round-trips a valid UTF-8 string unchanged
- [ ] A Spark program writes and reads a real binary file byte-exactly
- [ ] Suite + sanitizers green, `fib(27)` min unaffected (T109/T111 touch the VM)
- [ ] Review with the user before phase 23

---

## Phase 23 - Codecs, text, time, crypto and regex (P1, decision A17)

### T114 - `base64_encode`/`base64_decode`, `hex_encode`/`hex_decode`
**Description:** New `src/codec.c` with the four natives over `bytes` (accepting a
`string` as its UTF-8 bytes for convenience, returning `bytes`). Standard alphabets
only, with and without padding on decode.
**Acceptance criteria:**
- [ ] RFC 4648 test vectors pass, including the padding edges (`""`, `"f"`, `"fo"`,
      `"foo"`, `"foob"`, `"fooba"`, `"foobar"`) and a 1 MB round trip.
- [ ] Invalid input (bad alphabet character, truncated group, wrong padding) raises
      `ValueError`; `hex_decode` rejects odd-length input.
**Verification:**
- [ ] Fixtures with the RFC vectors; suite + sanitizers.
**Dependencies:** T108. **Files:** `src/codec.c`, `src/codec.h`, `src/vm.c`, `tests/`,
`docs/language.md`. **Scope:** S.

### T115 - Deflate/gzip decode (vendored inflate, Q17)
**Description:** Add `gunzip(bytes)` / `inflate(bytes)` so the HTTP client can decode
`Content-Encoding: gzip` and `deflate`. Q17 is decided: **vendor a minimal inflate**
(public-domain lineage, adapted into `src/codec.c`) instead of adding zlib, so the
"no external dependencies" rule in `AGENTS.md` needs no new exception, and record the
provenance and licence of the adapted code in the file header and `CHANGELOG.md`.
Decompression must be **bounded**: a maximum output size with a hard error, so a
decompression bomb cannot exhaust memory.
**Acceptance criteria:**
- [ ] Decoding a fixture gzip/deflate stream returns the exact original bytes; a
      truncated or corrupt stream raises `ValueError`/`NetworkError`, never hangs.
- [ ] An output cap (documented, default 16 MiB) is enforced with an explicit error,
      and a fixture with a zip bomb trips it.
- [ ] The adapted source's origin and licence are stated in the file header, and
      `AGENTS.md` still lists OpenSSL as the only third-party dependency.
**Verification:**
- [ ] Fixtures: pre-generated `.gz`/`deflate` blobs under `tests/` (no network), the
      bomb fixture, and a fuzz-ish loop over truncated inputs; suite + sanitizers.
**Dependencies:** T108, T114. **Files:** `src/codec.c`, `AGENTS.md`, `CHANGELOG.md`,
`tests/`, `docs/language.md`. **Scope:** M.

### T116 - `format` for printf-style text
**Description:** `format(template, args...)` with `%s %d %i %f %g %x %X %o %b %c %%`
and basic width/precision, returning a string. No new operator (the `%` operator is
modulo and stays modulo). Built on the existing string builder so it is linear.
**Acceptance criteria:**
- [ ] A missing or extra argument is an `ArityError`-style error naming the specifier
      index; an unknown specifier is a `ValueError` at runtime with a clear message.
- [ ] Integers use the int path (`%d` on a float truncates only when `is_int` is false
      and the float is integral, otherwise `TypeError`), matching `int()` semantics.
- [ ] Width/precision behave as documented, verified by fixtures including `%08.3f`.
**Verification:**
- [ ] Fixtures covering every specifier, the errors and a 100k-append benchmark;
      suite.
**Dependencies:** None (uses `string_builder`). **Files:** `src/vm.c` or
`src/text.c`, `tests/`, `docs/language.md`. **Scope:** S.

### T117 - Date and time formatting and parsing
**Description:** New `src/time.c`: `time_format(timestamp?, format)` (strftime subset:
`%Y %m %d %H %M %S %j %w %a %A %b %B %z %Z %%`), `time_parse(text, format)` returning a
Unix timestamp, `time_utc(time)` / `time_local(time)` for the offset, and `time_ms()`
for a millisecond clock. All in UTC by default so tests are deterministic.
**Acceptance criteria:**
- [ ] UTC is the default for both formatting and parsing; local-time behavior is
      explicit and documented (and fixtured against a fixed `TZ`).
- [ ] `time_parse(time_format(t, f), f) == t` for a table of timestamps, including
      leap-day (2024-02-29) and year boundaries.
- [ ] Out-of-range or unmatchable input raises `ValueError`; `%Z` handling is documented
      as best-effort.
**Verification:**
- [ ] Fixtures with `TZ=UTC` pinned in the harness; suite. No network, no locale
      dependence (documented).
**Dependencies:** None. **Files:** `src/time.c`, `src/time.h`, `src/vm.c`, `tests/`,
`docs/language.md`. **Scope:** M.

### T118 - `sha256`, `hmac_sha256`, `rand_bytes` through OpenSSL
**Description:** New `src/crypto.c` using the already-linked OpenSSL crypto library
(no new dependency): `sha256(bytes_or_string)` -> 32-byte `bytes`, `sha256_hex`,
`hmac_sha256(key, data)`, `rand_bytes(n)` from `RAND_bytes` (erroring if the CSPRNG
fails). Update `AGENTS.md`/`CHANGELOG.md` to say explicitly that the approved OpenSSL
dependency is now used for TLS **and** these primitives.
**Acceptance criteria:**
- [ ] NIST/FIPS test vectors pass for SHA-256 (including the empty string and the
      million-`a` vector at reduced size) and RFC 4231 vectors for HMAC-SHA256.
- [ ] `rand_bytes` returns distinct values across calls and errors instead of falling
      back to `rand()`; the language's `rand()` stays a non-cryptographic PRNG and the
      docs say which is which.
- [ ] No secret (key, input) is echoed in an error message.
**Verification:**
- [ ] Fixtures with the published vectors; suite + sanitizers; docs updated in both
      `AGENTS.md` and the API table.
**Dependencies:** T108 (for the `bytes` return type). **Files:** `src/crypto.c`,
`src/crypto.h`, `src/vm.c`, `AGENTS.md`, `CHANGELOG.md`, `tests/`, `docs/language.md`.
**Scope:** M.

### T119 - Regex: `{n,m}`, flags and match offsets
**Description:** The engine is a Pike VM (`src/regex.c`: `RE_SPLIT`/`RE_JUMP`,
thread-list simulation with leftmost-longest tie-breaking at `src/regex.c:559`).
Add: bounded repetition `{n,m}`/`{n}`/`{n,}` (unrolled by the compiler), inline and
argument flags (`i` case-insensitive, `s` dot-matches-newline, `m` multiline anchors —
`^`/`$` already see `\n` at `src/regex.c:512`), and match offsets so callers can
tokenize: `regex_find_all(pattern, text)` gains an index form, and a new
`regex_match_at`/`regex_find_index` returns start/end positions as integers.
**Acceptance criteria:**
- [ ] `{n,m}`, `{n}` and `{n,}` match per POSIX/Perl expectations, including `a{0,3}`,
      `(ab){2,3}` and the error case `a{3,2}`.
- [ ] `(?i)` and the flags argument both work, and the doc states which flags exist.
- [ ] Offsets are character-based (consistent with `len`/`index_of`) and verified on a
      multibyte string.
- [ ] Non-greedy quantifiers `*?`, `+?` and `??` **work** (Q27 is decided: move to
      Perl/JavaScript leftmost-first semantics), and the tie-breaking change is recorded
      in the `CHANGELOG.md` "Breaking changes" section. The existing fixtures that encode
      leftmost-longest (`regex_find("<.*>", ...)` and any alternation-in-group case) are
      updated in the same commit, with the before/after behavior noted in the task.
**Verification:**
- [ ] Fixtures per feature and per error; the multibyte offset fixture; suite +
      sanitizers. Reject pathological patterns with a bounded step/work limit so a
      crafted pattern cannot hang the VM (fixture included).
**Dependencies:** None. **Files:** `src/regex.c`, `src/regex.h`, `src/vm.c`, `tests/`,
`docs/language.md`. **Scope:** M.

### T120 - Regex: capture groups
**Description:** Add group capture to the Pike VM: thread-carried capture slots (the
RE2 approach) or an equivalent, exposed as `regex_match_groups(pattern, text)` ->
`{start, end, groups: [{start, end, text}], named: {...}}` plus named groups `(?P<n>)`.
Keep `regex_match`/`regex_find` as they are so existing programs keep their behavior
and their cost.
**Acceptance criteria:**
- [ ] Groups, nested groups, optional groups that did not participate (`nil`) and named
      groups all behave as documented, with the group count exposed.
- [ ] With N groups the thread state stays bounded: a fixture with 20 groups and a
      1 MB haystack does not blow memory or time.
- [ ] Unbalanced parentheses remain a `ValueError` with the existing message style.
**Verification:**
- [ ] Fixtures including "unmatched optional group is nil", nested captures and named
      captures; suite + ASan/UBSan (this code holds pointers into the haystack).
**Dependencies:** T119. **Files:** `src/regex.c`, `src/regex.h`, `src/vm.c`, `tests/`,
`docs/language.md`. **Scope:** M.

### T121 - Regex: `regex_split` and group references in `regex_replace`
**Description:** `regex_split(pattern, text, limit?)` and `$1`/`${name}` expansion in
`regex_replace` (with `$$` for a literal `$`), which today replaces with a literal
replacement string.
**Acceptance criteria:**
- [ ] `regex_split` handles leading/trailing/empty fields and the limit argument the
      way `split` does; the docs state the difference between the two.
- [ ] `regex_replace("(a)(b)", "$2$1", "ab")` is `"ba"`; `$$` yields `$`; an unknown
      group reference raises `ValueError` instead of producing an empty string.
- [ ] A small in-docs tokenizer example uses groups, offsets and split together and runs
      as written.
**Verification:**
- [ ] Fixtures; the example executed by the suite; suite green.
**Dependencies:** T120. **Files:** `src/regex.c`, `src/vm.c`, `tests/`,
`docs/language.md`. **Scope:** S.

### T122 - Math natives
**Description:** Add the missing numeric surface: `tan`, `asin`, `acos`, `atan`,
`atan2`, `log2`, `log10`, `cbrt`, `hypot`, `trunc`, `sign`, `clamp`, `gcd`, `lcm`,
`round(x, digits?)` (optional digits on the existing `round`), and the `inf` constant.
Match the existing convention: an integer input stays an integer where the operation
allows it (`abs`/`floor`/`ceil`/`round` already do this), and domain errors are
`ValueError` (`sqrt(-1)` is the existing precedent).
**Acceptance criteria:**
- [ ] Each native has a fixture with the documented type behavior (int stays int where
      stated) and its domain error.
- [ ] `round(x, digits)` is exact for the documented cases and documented as
      binary-float rounding (not decimal).
- [ ] `inf` behaves with `is_nan`/comparisons as documented (and `is_nan(inf)` is
      false).
**Verification:**
- [ ] Fixtures; suite green; the native list in the docs cross-checked against
      `vm_init`'s registration list.
**Dependencies:** None. **Files:** `src/vm.c`, `tests/`, `docs/language.md`, `README.md`.
**Scope:** S.

### T123 - String and collection conveniences
**Description:** Close the everyday gaps found in the audit: `count(container, value)`
and `count(text, substring)`, `split(text, sep, limit?)`, `trim(text, chars?)`,
`range(start, end, step)`, `sort(array, comparator?)` (accepting the `sort_by`
comparator so there is one sorting entry point), `shuffle(array)` and `choice(array)`
(seeded by `srand`), `enumerate`/`zip` already exist. Keep `sort_by` as an alias for
compatibility.
**Acceptance criteria:**
- [ ] `range` with a step validates `step != 0` and handles negative steps per the
      documented rule; existing two-argument calls are unchanged.
- [ ] `sort(array, cmp)` and `sort_by` produce identical results (a fixture asserts it)
      and `sort` stays stable.
- [ ] `shuffle` respects `srand` for reproducibility (fixture with a fixed seed) and
      uses the same PRNG as `rand`.
**Verification:**
- [ ] Fixtures per native and per error; `benchmarks/sort*.spark` still pass; suite.
**Dependencies:** None. **Files:** `src/vm.c`, `tests/`, `docs/language.md`, `README.md`.
**Scope:** M.

## Checkpoint: Phase 23
- [ ] Every native has a fixture and a reference-table row
- [ ] The tokenizer example in `docs/language.md` uses capture groups and offsets
- [ ] Suite + sanitizers green; `fib(27)` min unchanged

---

## Phase 24 - HTTP and network hardening (P1, decision A21)

### T124 - gzip through the HTTP client
**Description:** Send `Accept-Encoding: gzip, deflate` by default (a documented option
to disable it) and transparently decode `Content-Encoding` on the response, applying the
`max_body` cap **after** decompression so the bound still holds.
**Acceptance criteria:**
- [ ] A local server that answers gzip-compressed returns the decoded body; a server
      that answers identity still works.
- [ ] The decompressed size is capped by `max_body` with an explicit `NetworkError`;
      the bomb fixture from T115 is reused at the HTTP level.
- [ ] `Accept-Encoding` can be overridden through the `headers` argument without
      duplicating the header.
**Verification:**
- [ ] Fixtures against the local Python server (offline); suite + sanitizers.
**Dependencies:** T115. **Files:** `src/net.c`, `tests/`, `docs/language.md`.
**Scope:** S.

### T125 - Keep-alive and connection reuse
**Description:** Stop forcing `Connection: close` (`src/net.c:1767`) and reuse one
connection per `{scheme, host, port}` across requests, with `Connection: close` still
honored on request and respected when the server sends it, plus `Connection: keep-alive`
timeout handling and stale-connection retry on a safe (idempotent) request.
**Acceptance criteria:**
- [ ] N sequential requests to the same local host reuse one TCP connection (asserted by
      counting accepts on the server side).
- [ ] A server that closes the connection anyway does not break the next request.
- [ ] The `http` benchmark improves measurably against the baseline (157 ms min) and the
      number is recorded in the task.
**Verification:**
- [ ] Fixtures: reuse count, server-side close, stale retry; `benchmarks/run_all.py`
      before/after; suite + sanitizers.
**Dependencies:** T124. **Files:** `src/net.c`, `tests/`, `benchmarks/`,
`docs/architecture.md`. **Scope:** M.

### T126 - The HTTP client yields its coroutine
**Description:** Remove the documented "the HTTP client is synchronous" limitation
(`docs/language.md:731`, A21): route the HTTP client's socket operations through the
network waiter machinery so a request suspends only its coroutine, exactly like
`recv`/`send`. The connection state must be GC-safe across a suspension (the socket is
already a GC-managed object; the redirect/keep-alive bookkeeping must be traced too).
**Acceptance criteria:**
- [ ] One process makes several concurrent HTTP requests with `spawn` and none of them
      blocks the others (fixture with two coroutines against a slow local endpoint).
- [ ] A would-block HTTP call inside a native callback or an imported module raises the
      documented `run_depth > 1` error instead of suspending.
- [ ] Timeouts, redirects, chunked bodies and the gzip path keep their T92/T124
      behavior; the fd count returns to baseline after the fixture.
**Verification:**
- [ ] Fixtures: two concurrent requests, the callback error, a timeout mid-request,
      redirects with suspensions; suite + ASan/UBSan; `http` benchmark reported.
**Dependencies:** T125, T115. **Files:** `src/net.c`, `src/vm.c`, `tests/`,
`docs/language.md`, `docs/architecture.md`. **Scope:** M (open with a one-request spike,
as phase 16 did).

### T127 - HTTP server: concurrency, keep-alive, bounded request bodies
**Description:** `lib/http.spark` today accepts one connection at a time and reads
`Content-Length` bodies as text. Make it serve many connections from one coroutine
(using the existing scheduler-aware sockets), support keep-alive with a bounded idle
timeout, and read request bodies as `bytes` with a cap and `Transfer-Encoding: chunked`
support.
**Acceptance criteria:**
- [ ] 32 concurrent clients are served by one server coroutine with no busy-wait (the
      T90 fixture pattern) and no fd leak.
- [ ] Keep-alive serves several requests on one connection, closing on idle timeout.
- [ ] A body over the cap answers `413` and does not grow memory unboundedly; a chunked
      request body is parsed.
**Verification:**
- [ ] Fixtures with Spark's own client plus the Python harness; suite; sanitizers.
**Dependencies:** T111, T126. **Files:** `lib/http.spark`, `examples/http_server.spark`,
`tests/`, `docs/language.md`, `docs/architecture.md`. **Scope:** M.

### T128 - WebSocket keepalive/reconnect library helper
**Description:** Add `lib/ws_client.spark`: a thin layer over the native `websocket`
object that runs a heartbeat at an interval, reconnects with bounded exponential
backoff, and surfaces a clean error when the peer closes. It exists so the Discord
example (and any gateway client) does not hand-roll heartbeat timing.
**Acceptance criteria:**
- [ ] The helper reconnects after a peer-initiated close within the documented backoff
      and caps the attempts.
- [ ] `examples/discord_bot.spark` is refactored onto the helper and still passes the
      mock-gateway fixture unchanged.
- [ ] Secrets stay out of logs and errors (T99's rule).
**Verification:**
- [ ] Mock-gateway fixture (offline) covers the reconnect and the heartbeat; suite.
**Dependencies:** T113 (only for the server side of the fixture; the helper itself only
needs the existing client). **Files:** `lib/ws_client.spark`, `examples/`, `tests/`,
`docs/language.md`. **Scope:** S.

## Checkpoint: Phase 24
- [ ] A Spark program fetches a real gzip-compressed endpoint offline
- [ ] One process serves N concurrent HTTP clients without busy-waiting
- [ ] `http` and `echo` benchmarks measured and recorded (T125/T126/T127)
- [ ] Review with the user before phase 25

---

## Phase 25 - Language ergonomics and object protocols (P2, decision A23)

### T129 - `else` on loops
**Description:** `while (c) { ... } else { ... }` and the same for `for`/`for-in`: the
`else` block runs when the loop finished without `break`. Compiler-only change (jump
targets plus a "did we break" flag) with no new opcode if the existing jumps suffice.
**Acceptance criteria:**
- [ ] `else` runs on normal completion (including a zero-iteration loop) and does not
      run after `break`, including `break label` out of an outer loop.
- [ ] `continue` does not trigger `else`; nested loops only run the `else` of the loop
      that completed.
- [ ] The formatter and the linter understand the new syntax (they were caught out by
      earlier syntax additions).
**Verification:**
- [ ] Fixtures covering each case plus formatter/linter fixtures; suite + sanitizers.
**Dependencies:** None. **Files:** `src/parser.c`, `src/compiler.c`, `src/ast.h`,
`src/formatter.c`, `tests/`, `docs/language.md`. **Scope:** S.

### T130 - `break value`
**Description:** Allow `break expr;` (and `break label expr;`) to leave a loop with a
value, making `var found = while (...) { ... break x; };` impossible to express without
a sentinel. Restrict it to loops (not `switch`) or define `switch` behavior explicitly.
**Acceptance criteria:**
- [ ] The loop expression evaluates to the value only when a `break value` ran; the
      documented default is `nil` otherwise.
- [ ] `break label value` works; a bare `break` still yields `nil`.
- [ ] Type errors on the value are ordinary runtime errors at the loop site.
**Verification:**
- [ ] Fixtures for every combination; formatter/linter fixtures; suite + sanitizers.
**Dependencies:** T129 (both touch the same loop compilation path). **Files:**
`src/parser.c`, `src/compiler.c`, `src/ast.h`, `src/formatter.c`, `tests/`,
`docs/language.md`. **Scope:** S.

### T131 - Multiple assignment and swap
**Description:** `a, b = b, a` and destructuring assignment for existing targets
(`[a, b] = pair;`, `{x, y} = point;`) — declaration-time destructuring already exists,
this is the assignment form. Evaluate the right-hand side fully before assigning, so
swaps are correct.
**Acceptance criteria:**
- [ ] The swap, a three-way rotation and the destructuring forms all work, including
      with index and field targets (`a[0], b.k = 1, 2`).
- [ ] Arity mismatch is a documented error (too few/too many elements), not a silent
      `nil`.
- [ ] `const` targets still raise `ConstError`.
**Verification:**
- [ ] Fixtures for each form and error; suite + sanitizers.
**Dependencies:** None. **Files:** `src/parser.c`, `src/compiler.c`, `src/ast.h`,
`src/formatter.c`, `tests/`, `docs/language.md`. **Scope:** M.

### T132 - `__iter__`, `__call__`, `__contains__`, `__bool__`
**Description:** Four protocol hooks on the existing instance dispatch: `for (var x in
obj)` calls `__iter__` (returning an array or a coroutine-free iterator the VM can
advance), `obj(args)` calls `__call__` (error message becomes "instance is not callable"
when absent), `contains(obj, v)` consults `__contains__`, and truthiness consults
`__bool__` falling back to always-truthy. Each is optional and the absence of the hook
keeps today's behavior and error message.
**Acceptance criteria:**
- [ ] Each hook works, and its absence reproduces today's exact error/behavior
      (fixtures assert the current messages).
- [ ] `__iter__` results are iterated without per-element allocation and are traced by
      the GC while the loop runs (`gc()` inside the loop does not break it).
- [ ] `__bool__` must return a bool or a value with the documented truthiness; a wrong
      return type is a `TypeError` naming the method.
**Verification:**
- [ ] Fixtures per hook, per absence and per error; coroutine_gc-style fixture for
      iteration across `gc()`; suite + sanitizers.
**Dependencies:** None. **Files:** `src/vm.c`, `src/object.c`, `src/object.h`, `tests/`,
`docs/language.md`. **Scope:** M.

### T133 - `__hash__` and instance map keys
**Description:** Instances are currently keyed by identity, so two equal instances are
two keys. Add `__hash__` (with `__eq__` already supported) so instances become
value-keyed when a class defines it, keeping identity for classes that do not. Document
the invariant (equal means same hash) and what happens when a class defines `__eq__`
without `__hash__`.
**Acceptance criteria:**
- [ ] A class with both hooks works as a map key: two equal instances overwrite the same
      entry and `del`/`has` agree.
- [ ] A class with neither keeps identity semantics exactly as today.
- [ ] Mutating an instance used as a key does not corrupt the table (the documented rule
      plus a fixture showing the resulting miss, not a crash).
**Verification:**
- [ ] Fixtures; suite + ASan/UBSan (map internals).
**Dependencies:** T132 (same dispatch site). **Files:** `src/object.c`, `src/vm.c`,
`tests/`, `docs/language.md`. **Scope:** M.

### T134 - Reflected operators
**Description:** Operators currently consult only the left operand's method
(`docs/language.md:1106`). Add the reflected forms (`__radd__`, `__rsub__`, `__rmul__`,
`__rdiv__`, `__rmod__`, `__rpow__`, `__rlt__`, `__rgt__`, `__rle__`, `__rge__`) for the
case where the left operand is a primitive and the right one is an instance — the
classic `2 * vector` case.
**Acceptance criteria:**
- [ ] `2 * v` calls `v.__rmul__(2)`; when both sides can handle it, the left operand
      still wins (no behavior change for existing programs).
- [ ] `a == b` keeps its current rule (right side consulted only when the left does not
      define `__eq__`, and `nil` comparisons stay identity).
**Verification:**
- [ ] Fixtures for each reflected operator, the precedence rule and the `nil` exception;
      suite + sanitizers.
**Dependencies:** None. **Files:** `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** S.

### T135 - Error values: `trace` and a built-in `Error` hierarchy
**Description:** A caught error is a map with `type`, `message`, `line`, `column`
(`src/vm.c:3787`), with no stack trace and no class to test against. Add a `trace`
field (array of `{function, line, column}`, capped, no secrets) and built-in error
classes (`Error` and one subclass per category: `TypeError`, `NetworkError`, ...) so
`catch (e) { if (e instanceof NetworkError) ... }` works. Keep the map form working for
compatibility, or document the change (A19).
**Acceptance criteria:**
- [ ] `e.trace` lists the call frames for a nested failure, capped at a documented depth
      and truncated with a marker.
- [ ] Every existing category is reachable through `instanceof`, and the string
      category names keep working (`e.type` is unchanged).
- [ ] No error value echoes request bodies, headers or token-looking strings (T99's rule
      re-verified).
**Verification:**
- [ ] Fixtures: trace caps, nested frames, `instanceof` per category, the redaction case;
      suite + sanitizers.
**Dependencies:** None. **Files:** `src/vm.c`, `src/error.c`, `src/object.c`, `tests/`,
`docs/language.md`, `docs/architecture.md`. **Scope:** M.

### T136 - `to_json` for instances and pretty printing
**Description:** `to_json` currently rejects instances with `cannot serialize instance
to JSON`. Add a `to_json()` protocol method on classes (returning a serializable value),
a documented fallback (public fields as an object) and an optional indentation argument
for pretty output. Keep the depth limit and make the circular-reference error message
name the cycle instead of just the depth.
**Acceptance criteria:**
- [ ] An instance with `to_json()` serializes through it; without it, the documented
      fallback applies (fields, excluding statics and methods).
- [ ] `to_json(value, 2)` pretty-prints with stable key ordering for maps (see T137) and
      the output re-parses with `from_json` to an equal value.
- [ ] A cycle raises `ValueError` naming the repeated object, and functions/natives keep
      their current error.
**Verification:**
- [ ] Fixtures: protocol, fallback, pretty round trip, cycle, depth cap; suite.
**Dependencies:** T137 for stable key order in pretty output (can land after it).
**Files:** `src/json.c`, `src/vm.c`, `tests/`, `docs/language.md`. **Scope:** S.

### T137 - Insertion-ordered maps (decided: Q22)
**Description:** `keys(m)` returns hash order today (`{"b":1,"a":2}` -> `["a","b"]`),
which surprises anyone coming from Python/Ruby/JS. Per Q22, `map` itself adopts insertion
order for iteration, `keys`, `values`, `enumerate` and JSON output, keeping the hash
table for lookup (order metadata in the entry chain, as Python's compact dict does). No
separate ordered type is added.
**Acceptance criteria:**
- [ ] Iteration order is specified behavior and documented in `docs/language.md`;
      `{"b":1,"a":2}` iterates as `b`, `a`.
- [ ] Iteration, `keys`, `values` and JSON follow insertion order; `del` then re-insert
      moves the key to the end (documented).
- [ ] Deletion does not degrade lookup: a 1M-insert/1M-delete/1M-insert fixture keeps the
      `maps` benchmark within a few percent of the baseline.
**Verification:**
- [ ] Order fixtures (insert, delete, re-insert, shrink, clear); `benchmarks/maps.spark`
      before/after; suite + ASan/UBSan.
**Dependencies:** None. **Files:** `src/object.c`, `src/object.h`, `src/vm.c`,
`src/json.c`, `tests/`, `docs/language.md`. **Scope:** M.

### T138 - Getters and `enum`
**Description:** Two ergonomic additions, both approved by Q23: computed properties
(`get x() { ... }` on a class, invoked on field read, with `self.x` bypassing it only
through a documented accessor) and `enum` declarations (`enum Color { RED, GREEN }`
producing instances with `name`/`value`, `to_string()`, and a lookup by name/number).
`private`/`readonly` fields and `defer` are out of 0.2 (Q23) and must not be smuggled in
here.
**Acceptance criteria:**
- [ ] A getter is invoked on `obj.x` read and never on assignment; a getter that throws
      propagates a normal error; `len`/`str` interplay is unchanged.
- [ ] `enum` members are stable, printable, comparable, and usable with `switch`; an
      unknown member lookup raises the documented error.
- [ ] Both forms are understood by the formatter and the linter, with fixtures.
**Verification:**
- [ ] Fixtures for both features, their errors and their formatter/linter support;
      suite + sanitizers; docs updated.
**Dependencies:** None. **Files:** `src/parser.c`, `src/compiler.c`, `src/vm.c`,
`src/object.c`, `src/formatter.c`, `tests/`, `docs/language.md`. **Scope:** M.

## Checkpoint: Phase 25
- [ ] Every new protocol/feature has positive and error fixtures
- [ ] `docs/language.md` sections 3-9 and 14 list every operator and protocol
- [ ] Suite + sanitizers + benchmark gate green; review with the user before phase 26

---

## Phase 26 - Performance (P2/P3, all measured, decisions A18/A20)

> Every task here states its target and reports before/after numbers from
> `python3 benchmarks/run_all.py` (min of 7). No optimization lands on a hunch, and
> none of T141/T143/T148 lands in the same commit as another one.

### T139 - Object allocator: arena and free lists
**Description:** Every object is a separate `realloc` (`src/memory.c:34`) plus a
linked-list insert. Introduce a size-classed arena with free lists for the fixed-size
object headers and small buffers, keeping the `mem_*` seam so the change is revertable
and `mem_allocated()` stays accurate for the GC threshold.
**Acceptance criteria:**
- [ ] `alloc` (1M arrays) min improves at least 20% against the 74.87 ms baseline.
- [ ] `fib(27)` min does not regress; `maps`/`arrays`/`strings` improve or stay flat.
- [ ] ASan/UBSan clean and no leak reported by the suite's fd/memory checks.
**Verification:**
- [ ] `run_all.py` before/after with the numbers in the task; suite + sanitizers; the
      numbers in `CHANGELOG.md`.
**Dependencies:** None. **Files:** `src/memory.c`, `src/memory.h`, `src/object.c`,
`docs/architecture.md`. **Scope:** M.

### T140 - GC observability and tuning
**Description:** Add `gc_stats()` (collections, bytes allocated, live bytes, threshold,
pause time of the last cycle) and `SPARK_GC_*` environment variables (initial
threshold, growth factor, collector off) so the collector can be observed and the
performance work can be attributed. Without this, T141 cannot be measured honestly.
**Acceptance criteria:**
- [ ] `gc_stats()` reports values that match `--gc-stats` on exit and are documented.
- [ ] The env vars work, are documented for embedders, and their absence keeps today's
      adaptive behavior identical.
- [ ] A fixture asserts the counters move in the expected direction (allocate, collect,
      see live bytes drop).
**Verification:**
- [ ] Fixtures; suite; docs in `docs/language.md` and `docs/architecture.md`.
**Dependencies:** T139 (so the numbers mean something). **Files:** `src/memory.c`,
`src/vm.c`, `src/main.c`, `tests/`, `docs/`. **Scope:** S.

### T141 - Generational GC with a nursery
**Description:** The collector is a full mark-sweep over the whole heap with an adaptive
threshold (1.5x-2.5x live). Add a generational scheme: a nursery for young objects, a
minor collection with remembered-set or card-marking of old-to-young pointers, and a
major collection that reuses the current code. Respect the existing constraints: the
gray stack, the intern table's weak sweep (`intern_remove_unmarked`), coroutine stacks
and open upvalues as roots, and `mem_pause_gc()` regions.
**Acceptance criteria:**
- [ ] `alloc` and `maps` min improve at least 25% against the T139 numbers, and
      `fib(27)` min stays under the 5 ms gate. Q26 is decided: the gate does not move,
      so a design that cannot hold it is revised or reverted rather than accepted.
- [ ] The existing GC-sensitive fixtures pass: `coroutine_gc`, the channel/coroutine
      ones, `string_builder_scale`, plus the interning and weak-table paths.
- [ ] A stress fixture (allocate in a loop while holding coroutines, channels and
      blocked socket waits) completes with ASan/UBSan clean.
**Verification:**
- [ ] `run_all.py` before/after; suite on optimized and sanitizer builds; the design and
      the remembered-set choice recorded in `docs/architecture.md`.
**Dependencies:** T140. **Files:** `src/vm.c`, `src/memory.c`, `src/object.c`,
`docs/architecture.md`. **Scope:** L (open with a spike that measures a minor
collection only; split further if the spike is not conclusive).

### T142 - Superinstructions for the global path and the loop idiom
**Description:** The fused ops cover locals only, but top-level code uses globals: in
`int_loop` the accumulator is `GET_GLOBAL`/`SET_GLOBAL` and the increment is two ops
(`ADD_LOCAL_CONST` + `SET_LOCAL`). Add (a) a fused increment (`i += 1` in one op),
(b) `OP_*_LOCAL_LOCAL` arithmetic, (c) `INDEX_INC` for `a[k] += 1` (the `++` path exists,
`+= 1` does not: `src/vm.c:6467`), and (d) `GET_GLOBAL` + `CALL` fusion for the common
call shape. Follow the opcode checklist in `AGENTS.md` section 5 (enum, `opcode_name`,
disassembler, `run()`, fixture).
**Acceptance criteria:**
- [ ] `int_loop` min improves at least 15% against the baseline and `fib(27)` improves or
      stays flat.
- [ ] Every new opcode is in the enum, `opcode_name`, the disassembler and a fixture.
- [ ] The stack is balanced on every path, verified by the sanitizer run and by a
      fixture per fused op.
**Verification:**
- [ ] Disassembly before/after for the benchmark loop, `run_all.py` numbers, suite +
      sanitizers.
**Dependencies:** None. **Files:** `src/chunk.h`, `src/chunk.c`, `src/compiler.c`,
`src/vm.c`, `tests/`. **Scope:** M.

### T143 - Script scoping for top-level variables (files only, Q21)
**Description:** Top-level variables are globals, which costs a global load/store per
access and bypasses every fused op. Per Q21/A28, compile them as locals of the main
function **when the unit is a file and nothing captures them**; the REPL keeps global
semantics (its variables persist across lines) and any name an imported module can
observe stays global, since the module system shares one namespace
(`docs/architecture.md:565`).
**Acceptance criteria:**
- [ ] The module and REPL cases are fixtured **before** the change: a function in module
      A reading a top-level variable of module B keeps working, and a REPL session keeps
      its variables across lines.
- [ ] A name that an importing module can see is not silently demoted to a local.
- [ ] A top-level loop benchmark improves at least 20% and no semantic fixture changes.
**Verification:**
- [ ] New fixtures for module cross-access, REPL, closures over top-level names and
      `eval`-free dynamic patterns; `run_all.py` numbers; suite + sanitizers.
**Dependencies:** None (Q21 decided). **Files:** `src/compiler.c`, `src/vm.c`, `tests/`,
`docs/architecture.md`. **Scope:** M (high risk: open with a spike on one benchmark).

### T144 - Scheduler: remove the extra `poll(2)` per turn
**Description:** `vm_scheduler` calls `net_scheduler_poll(false)` on every scheduling
step whenever a socket waiter exists (`src/vm.c:5028`), so an echo round trip pays extra
`poll` syscalls even when the next coroutine is already runnable. Poll only when the
next step cannot make progress, or track readiness so a known-ready descriptor skips the
syscall entirely.
**Acceptance criteria:**
- [ ] `echo` (2k RTT) min improves at least 15% against the 440.29 ms baseline; the
      concurrency and no-busy-wait fixtures from T90 still pass.
- [ ] An idle wait still uses no CPU (the T87/T90 property) and deadlines still fire
      within the documented slack.
**Verification:**
- [ ] `run_all.py` echo before/after; the 32-client fixture and the timer fixtures; suite.
**Dependencies:** None. **Files:** `src/vm.c`, `src/net.c`, `docs/architecture.md`.
**Scope:** S.

### T145 - Cheap coroutines: smaller initial stacks
**Description:** Each coroutine allocates a 2048-slot stack (`src/vm.c:4954`) which is
32 KB with the current 16-byte `Value`, so thousands of coroutines cost hundreds of MB.
Start small (for example 128 slots) and grow through the existing `grow_stack` path,
which already updates the owning coroutine's fields.
**Acceptance criteria:**
- [ ] 10,000 live coroutines cost under 100 MB total and complete correctly (fixture).
- [ ] A deep-recursion coroutine still grows to the documented limit and reports
      `RecursionError` at the same depth.
- [ ] `closures`/`coroutine` fixtures and the benchmark gate are unchanged.
**Verification:**
- [ ] Fixture with 10k coroutines plus the memory assertion; suite + sanitizers.
**Dependencies:** None. **Files:** `src/vm.c`, `tests/`, `docs/language.md` (limits).
**Scope:** S.

### T146 - Build: LTO and PGO targets
**Description:** The `Makefile` builds with `-O3 -g` only. Add an opt-in LTO build and a
`make pgo` target (instrumented build, run `fib` plus `run_all.py` as the profile, then
rebuild with the profile data), keeping the default build unchanged so CI stays
reproducible.
**Acceptance criteria:**
- [ ] `make lto` and `make pgo` produce a working `spark`, measured against the default
      build on `fib`/`int_loop`; numbers recorded.
- [ ] The default `make` and the CI flags are untouched; `make clean` removes the new
      artifacts.
**Verification:**
- [ ] `make clean && make lto`, `make pgo`, then the suite on each; numbers in the task
      and `docs/architecture.md`.
**Dependencies:** None. **Files:** `Makefile`, `.github/workflows/ci.yml` (optional),
`docs/architecture.md`. **Scope:** S.

### T147 - Benchmark suite: function-body variants and a recorded baseline
**Description:** Most benchmarks run at the top level, so they exercise the global path
and under-report in-function speed (and they will change meaning under T143). Add
function-body variants of the loop benchmarks, keep the top-level ones, and record the
full min/median table plus the machine description in the plan as the regression
reference.
**Acceptance criteria:**
- [ ] Each benchmark has a documented "what it measures" line and the top-level/function
      split is explicit in `run_all.py` output.
- [ ] `benchmarks/run.py` keeps the `fib(27)` gate; `run_all.py` still runs offline in
      CI within the existing budget.
- [ ] The baseline table in `tasks/plan.md` is updated with the new variants before any
      T139+ change lands.
**Verification:**
- [ ] `python3 benchmarks/run_all.py` and the CI job; suite green.
**Dependencies:** Should land before T139-T143 to give them a baseline. **Files:**
`benchmarks/`, `tasks/plan.md`, `docs/architecture.md`. **Scope:** S.

### T148 - NaN-boxing of `Value` (Q20: go/no-go after T147)
**Description:** `Value` is a 16-byte tag+union (`src/value.h:19`); NaN-boxing halves it
and every stack slot, map entry and array element. This touches every hot path
(`AS_INT`, `numeric_fast`, `values_equal`, the disassembler's constant pool, the
bytecode constant format). Per Q20 this is an explicit go/no-go point after T147: the
decision is taken with the T139-T147 numbers in hand, and **dropping this task is a
valid outcome** that closes the performance phase.
**Acceptance criteria:**
- [ ] The go/no-go decision is recorded in `tasks/plan.md` and `CHANGELOG.md` with the
      numbers that justify it, before any implementation.
- [ ] A spike boxes only the stack and measures `fib`/`int_loop`/`maps`; the full change
      lands only if the spike shows a win at least as large as the allocator work.
- [ ] If it lands: suite + sanitizers green, no behavior change observable from Spark
      (the whole suite is the guard, plus the bytecode format version bump).
**Verification:**
- [ ] Spike numbers; full `run_all.py` before/after; suite on optimized and sanitizer
      builds.
**Dependencies:** T139-T147. **Files:** `src/value.h`, `src/object.h`, `src/vm.c`,
`src/bytecode.c`, `src/chunk.c`, `docs/architecture.md`. **Scope:** L (split if
approved: value macros, then the VM hot paths, then the constant format).

## Checkpoint: Phase 26
- [ ] `fib(27)` min < 5 ms with headroom restored (target: < 4.5 ms)
- [ ] `alloc`, `maps`, `arrays`, `int_loop`, `echo` measured against the baseline table
- [ ] Every optimization backed by before/after numbers in the task and `CHANGELOG.md`
- [ ] Review with the user before phase 27

---

## Phase 27 - Tooling and ecosystem (P3, decisions A24/A25)

### T149 - CLI: `--check`, `-e`, `--time`, `--gc-stats`, `--`
**Description:** `src/main.c` accepts nine flags (`src/main.c:395`). Add `--check`
(parse and compile only, no execution), `-e/--eval <code>` (run a string, with `argv`
set like a file), `--time` (wall time and peak memory for a run), `--gc-stats` (collector
summary on exit) and `--` to stop option parsing so a script argument starting with `-`
works. Update `--help` and the README usage section.
**Acceptance criteria:**
- [ ] Each flag works, has a fixture, and `--help` documents it; unknown flags still
      exit 64.
- [ ] `--check` on a file with a runtime-only error (for example an undefined global)
      exits 0, and on a syntax error exits 65.
- [ ] `spark -- script.spark -x` passes `-x` to `argv`.
**Verification:**
- [ ] CLI fixtures in the suite; suite green.
**Dependencies:** None. **Files:** `src/main.c`, `tests/`, `README.md`, `docs/language.md`.
**Scope:** S.

### T150 - REPL: line editing and history
**Description:** The REPL reads with `getline` (`src/main.c:340`), so there is no
history and no arrow-key editing. Add line editing with history. Per A22/Q17 there is no
new dependency, so the route is a small raw-mode editor (a few hundred lines: cursor
movement, insertion, history ring) rather than `libreadline`; if that proves too costly,
the fallback is to keep `getline` and document that the REPL has no line editing, rather
than adding a dependency.
**Acceptance criteria:**
- [ ] Up/down cycle history, left/right and home/end edit, Ctrl+C cancels the current
      line without exiting, Ctrl+D exits.
- [ ] Multi-line accumulation (unclosed braces) still works, and the `_` variable and
      expression printing are unchanged.
- [ ] `AGENTS.md` still lists OpenSSL as the only dependency; the build is warning-free
      with and without a TTY, and piping a script into the REPL still works.
**Verification:**
- [ ] Manual checklist documented in the task plus an automated test driving the REPL
      through a pipe for the non-TTY path; suite green.
**Dependencies:** None (A22/Q17 decided). **Files:** `src/main.c`, new editor file,
`Makefile`, `README.md`. **Scope:** M.

### T151 - Linter: undefined globals and unused imports
**Description:** The linter has three rules (`src/linter.c`): unused variable,
unreachable code, shadowing. Add `undefined-global` (a name that is neither a local, a
parameter, a declared global, nor a known native) and `unused-import`, plus a `--lint`
exit code contract check. Keep the rules conservative so valid dynamic patterns are not
flagged.
**Acceptance criteria:**
- [ ] `print(nope)` is flagged at lint time; a name defined later in the same file is
      not flagged; every native registered in `vm_init`/`net_init` counts as defined.
- [ ] An import whose module declares nothing used is flagged; a module imported for its
      side effects can be silenced with a documented convention.
- [ ] Exit codes stay as documented (0 clean, 1 warnings, 65 syntax, 74 unreadable).
**Verification:**
- [ ] Linter fixtures for each rule and each false-positive guard; suite green.
**Dependencies:** None. **Files:** `src/linter.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T152 - tree-sitter grammar, queries and editor files (decided: Q25)
**Description:** Ship a `tree-sitter-spark` grammar covering the whole language
(including the syntax from T129-T138), a `queries/` set for highlights and locals, and
editor configuration notes (Neovim, VS Code, Helix) in the README. Per Q25/A25 this is
the whole of the editor story for 0.2: no LSP, which is why the grammar must be complete
rather than approximate. A grammar is a real deliverable: it also gives the
formatter/linter a second parser to check against.
**Acceptance criteria:**
- [ ] The grammar parses every file in `tests/cases`, `examples/`, `lib/` and
      `benchmarks/` without error, asserted by a script in the suite.
- [ ] `tree-sitter test` corpus covers statements, expressions, classes, coroutines,
      modules and the new syntax.
- [ ] The README documents installation for at least one editor.
**Verification:**
- [ ] `tree-sitter test` plus the parse-everything script; suite green.
**Dependencies:** T129-T138 (the grammar should cover the final syntax). **Files:**
new `tree-sitter-spark/` directory, `tests/`, `README.md`. **Scope:** M.

### T153 - Public embedding header and library build
**Description:** `vm.h` exposes `vm_define_native` and friends but there is no public
header, no documented struct, and no library target: an embedder must edit `src/vm.c`.
Add `src/spark.h` (init/free, interpret a file or a string, register a native, get the
last error, set stdout hooks), a `make lib` target producing a static and a shared
library, and an `examples/embed.c` that runs a script. Mark it experimental (A24).
**Acceptance criteria:**
- [ ] `examples/embed.c` builds against the installed header and library and runs a
      script that calls a native registered by the embedder.
- [ ] The header does not leak internal types (`ObjString`, `Chunk`) beyond opaque
      pointers, and compiles standalone with `-Wall -Wextra -Wpedantic`.
- [ ] The docs state the API is not ABI-stable before 1.0 and list what is frozen.
**Verification:**
- [ ] Build the example in CI; suite green.
**Dependencies:** None. **Files:** `src/spark.h`, `Makefile`, `examples/embed.c`,
`.github/workflows/ci.yml`, `README.md`, `docs/architecture.md`. **Scope:** M.

### T154 - Module namespaces and `import ... as` (decided: Q24)
**Description:** Modules share one global namespace (`docs/architecture.md:565`), so two
libraries collide and nothing has a qualified name. Per Q24/A29 this lands in 0.2: add
`import "./x" as x;` with a per-module namespace object, keep the current behavior for a
plain `import` (A19), and define how a module exports (`export fn`/`export var`, or "all
top-level declarations").
**Acceptance criteria:**
- [ ] Plain `import` keeps working exactly as today (fixture), and a namespaced import
      cannot see another module's names by accident.
- [ ] `import "./x" as x;` makes `x.name` resolve without polluting the global namespace,
      and a name collision between two namespaced modules no longer breaks the program.
- [ ] Cyclic imports, `realpath` canonicalization and the once-only execution rule still
      hold for the namespaced form.
**Verification:**
- [ ] Fixtures: namespace isolation, collision, cyclic, `.sparkc` module, REPL; suite +
      sanitizers.
**Dependencies:** None (Q24 decided). **Files:** `src/parser.c`, `src/compiler.c`,
`src/vm.c`, `tests/`, `docs/language.md`, `docs/architecture.md`. **Scope:** M.

---

## Deferred to a later stream (decided, not 0.2)

### T155 - Project manifest and minimal package manager (deferred by Q24/A29)
**Description:** There is no way to declare a project or depend on a library. Add
`spark.toml` (name, version, entry point, dependencies by path or git ref with a
revision pin), `spark run` reading it, and a lockfile so installs are reproducible. No
central registry in this task (that is a separate stream). **Not part of 0.2**: Q24 keeps
namespaces in this release (T154) and moves packaging to its own stream, because it adds
git and network access to the surface the test suite must cover. The detail is kept here
so the agreed scope survives.
**Acceptance criteria:**
- [ ] Scope is a manifest, a resolver over path/git deps, a lockfile and
      `spark run`/`spark install`.
- [ ] Two projects depending on different revisions of the same module install side by
      side without one clobbering the other.
- [ ] Errors (missing manifest, unsatisfiable rev, checksum mismatch) are categorized and
      tested; nothing reaches the network in the test suite beyond a local git repo.
**Verification:**
- [ ] Fixtures using a local git repo and a local path dependency; suite green offline.
**Dependencies:** T154, Q24. **Files:** new `src/pkg.c` + `src/package.*`, `Makefile`,
`tests/`, `README.md`, `docs/`. **Scope:** L (split into manifest/`run`, resolver+lock,
then install if needed).

### T156 - Spark standard library modules in `lib/`
**Description:** `lib/` holds one file (`http.spark`) while everything else is a native.
Add the modules a program actually needs at the Spark level: `args` (argv parsing with
flags), `log` (levels, timestamps, stderr output), `collections` (helpers over the
natives), `test` (a tiny assert/group runner so the suite can host Spark-level tests) and
`fs` (path joining, walking, temp files as a thin native-plus-Spark layer). Each module
ships with fixtures and a docs section.
**Acceptance criteria:**
- [ ] Each module is documented with examples that run as written, and is lint-clean.
- [ ] `lib/test.spark` can express the existing style of assertion and is used by at
      least one Spark-level test file wired into `run_tests.py`.
- [ ] No module reaches the network in tests.
**Verification:**
- [ ] Fixtures per module; `python3 tests/run_tests.py` green.
**Dependencies:** T123, T137 (modules assume the collection conveniences), T111.
**Files:** `lib/*.spark`, `tests/`, `README.md`, `docs/language.md`. **Scope:** M.

### T157 - Parser/lexer fuzz targets and a CI job
**Description:** The suite relies on ASan/UBSan over fixed inputs; add fuzz targets for
the lexer, the parser/compiler and the bytecode loader (a truncated `.sparkc` is a real
attack surface), plus a short CI job that runs them on a time budget.
**Acceptance criteria:**
- [ ] Targets build with clang/libFuzzer (and a documented AFL path) behind a `make fuzz`
      target that is off by default.
- [ ] Bounded CI run of a few minutes per target finds no crash on the current tree; a
      crash corpus path is documented.
- [ ] The bytecode loader rejects malformed input with a normal error, never a crash
      (fixture with truncated/hostile `.sparkc` files).
**Verification:**
- [ ] CI job log; the hostile-bytecode fixtures; suite green.
**Dependencies:** None. **Files:** new `fuzz/`, `Makefile`,
`.github/workflows/ci.yml`, `tests/`. **Scope:** S.

### T158 - Coverage threshold and multi-platform release artifacts
**Description:** CI generates a coverage summary with no threshold, and releases publish
a single Linux binary. Set a floor (measured first, then enforced with `--fail-under` on
a meaningful subset such as `src/vm.c` and the new modules) and publish macOS artifacts
in the release workflow, or document explicitly that only Linux is supported.
**Acceptance criteria:**
- [ ] The threshold is based on a measured baseline (no guessed number) and fails CI when
      a change drops it.
- [ ] Either macOS binaries are built and smoke-tested in CI, or the README states the
      supported platforms plainly.
- [ ] The release workflow's checks (suite, sanitizers) stay mandatory.
**Verification:**
- [ ] CI run with the threshold; a deliberate coverage drop fails as expected (checked
      once, then reverted).
**Dependencies:** None. **Files:** `.github/workflows/*.yml`, `Makefile`, `README.md`.
**Scope:** S.

## Checkpoint: Complete
- [ ] All acceptance criteria met; suite, sanitizers and benchmark gates green
- [ ] `docs/language.md`, `docs/architecture.md`, `README.md`, `AGENTS.md`,
      `CHANGELOG.md` and `docs/intent/spark-0.2.md` match the code
- [ ] Ready for review
