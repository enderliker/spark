# Changelog

All notable changes to Spark are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Coroutines and channels: `spawn(fn, args...)`, the `yield` statement,
  `c.join()`, `c.done()`, `channel()` (rendezvous) and `channel(n)` (buffered),
  plus `ch.send(value)`, `ch.receive()` and `ch.close()`.
- The main script now runs as a coroutine, so it can `yield` and use channels.
- Channel waiters are traced by the collector: blocked coroutines, their stacks
  and the locals they captured stay alive across `gc()`.
- Networking: `tcp_connect`, `tcp_listen`, `udp_bind`, `net_hostname` and
  `net_select`, plus `socket` and `server` GC-managed resource objects. TCP
  clients and servers and UDP datagrams, with timeouts, IPv4/IPv6 resolution
  (bracketed IPv6 literals included) and categorized `NetworkError`/`TimeoutError`
  failures.
- A socket call that would block suspends its coroutine: `accept`, `recv`,
  `recv_line` and `send` are routed through the scheduler, which polls the
  registered descriptors (`poll(2)`, sleeping until the nearest deadline), so one
  process serves many connections with `spawn` and no busy-wait. The
  `run_depth > 1` guard extends to these waits.
- URL helpers: `url_parse` (scheme, host, port, path, query, fragment),
  `url_encode`/`url_decode` (percent-encoding and `+`) and `parse_query` (a map
  from key to an array of values).
- An HTTP/1.1 client: `http_get`, `http_post`, `http_put`, `http_delete` and the
  map-driven `http_request` return `{status, headers, body}` and handle chunked
  bodies, `Content-Length`, read-to-EOF and redirects, within explicit header
  (64 KiB), header-count, redirect (5), body (16 MiB) and timeout (30 s) limits.
  Connections are persistent: requests to one origin reuse a single TCP or TLS
  connection (`Connection: close` is honored in both directions, a connection the
  server closed anyway costs one retry on a request that is safe to repeat, a
  connection is only reused under the same `ca_file`, and idle ones are dropped
  after 30 s), which takes the `http` benchmark's 200 requests from 50.5 ms to
  14.6 ms (min of 7, WSL2).
- `lib/http.spark`: an HTTP/1.1 server written in Spark on the socket layer, with
  request parsing and a handler callback, used by `examples/http_server.spark`.
- TLS: `tls_connect(host, port, timeout_ms?, ca_file?)` returns a `socket` with
  `read`, `write` and `close` methods. The handshake and the reads/writes suspend
  their coroutine like the other socket operations. The chain is verified against
  the system trust store (or an explicit CA file) and the host name or IP address is
  checked, with SNI for DNS names and TLS 1.2 as the minimum version. There is no
  option to disable verification.
- HTTPS: `http_get`/`http_post`/`http_put`/`http_delete` accept `https://` URLs, and
  `http_request` gains a `ca_file` option for tests and private CAs. Certificate and
  host-name failures raise the same `NetworkError` as `tls_connect`.
- Test fixtures: throwaway CA, leaf, wrong-host, untrusted-chain and self-signed
  certificates under `tests/tls/`, with a regeneration script. The suite runs local
  TLS servers only and never touches the public internet.
- WebSocket: `ws_connect(url, timeout_ms?, ca_file?)` speaks RFC 6455 over `ws://`
  and `wss://`. It verifies the upgrade handshake, masks client frames and supports
  text, binary, ping/pong and the close handshake; fragmented messages are
  reassembled and protocol violations (masked server frames, reserved bits, orphan
  continuations, invalid UTF-8, invalid close codes) close with `1002`/`1007`/`1009`.
  A frame or message is capped at 16 MiB and a control payload at 125 bytes.
  `receive` suspends the coroutine through the scheduler and accepts a per-call
  timeout, so a heartbeat loop can time out and retry.
- WebSocket server: `ws_accept(socket, timeout_ms?)` upgrades a socket from
  `tcp_listen(...).accept()` after validating the RFC 6455 opening request and
  answering `101 Switching Protocols`. It returns the same `websocket` object as
  the client (text, binary, ping/pong, fragmentation and the close handshake),
  sends unmasked server frames, rejects unmasked client frames with `1002` and
  enforces the same 16 MiB message, 125-byte control and 123-byte close-reason
  limits. The handshake is synchronous and bounded by its timeout.
- `examples/ws_echo_server.spark` and `examples/ws_echo_client.spark`, run against
  each other by the suite.
- `examples/discord_bot.spark`: a Discord-shaped gateway client that identifies,
  heartbeats and answers a message event. `tests/run_tests.py` runs it against a
  local mock gateway (HELLO, IDENTIFY, READY, MESSAGE_CREATE), so the example is
  covered offline and never contacts Discord.
- `examples/http_client.spark` and `examples/echo_server.spark`, exercised by the
  suite against local counterparts. The suite also lints every example.
- Network benchmarks: `benchmarks/http_roundtrip.spark` joins the TCP echo
  benchmark in `benchmarks/run_all.py`, which starts a loopback HTTP server in a
  separate process. Both benchmarks run offline on ephemeral ports.
- A `bytes` type for binary data: `bytes(n)` (n zero bytes) and
  `bytes_from(array_or_string)` build an immutable byte sequence; `len(b)` counts
  bytes, `type(b)` is `"bytes"`, and `print`/`str` show a preview with
  non-printable bytes escaped. Equal bytes compare equal and are the same map key
  even as different objects. There is no implicit conversion between `string` and
  `bytes` (decision A16).
- `bytes` integrates with the operations that are type-directed: `b[i]` and
  negative indexing yield the byte value as an integer, `b[start:end:step]` and
  `slice(b, start, end?)` return new bytes with the same clamping as arrays,
  `for (var byte in b)` iterates without allocating per element (and the
  two-variable form yields offset and value), `contains`/`index_of`/`in` take an
  integer byte value or a bytes subsequence, and `+` concatenates two bytes
  values. Mixing bytes with text in any of these is a `TypeError` naming
  `bytes_from`. JSON encoding of `bytes` follows in a later task of the same
  phase.
- `bytes_builder()` (with `bb_append`, `bb_to_bytes`, `bb_len`, `bb_clear` and the
  `append`/`to_bytes`/`len`/`clear` methods) is the mutable companion of `bytes`,
  mirroring `string_builder()`.
- Codecs over `bytes`: `base64_encode`/`base64_decode` (RFC 4648, padding optional
  on decode) and `hex_encode`/`hex_decode` (lowercase output, either case on
  decode). Each accepts a string as its UTF-8 bytes and returns `bytes`; invalid
  input is a `ValueError`.
- DEFLATE and gzip decoding: `inflate(data, max?)` (RFC 1951) and
  `gunzip(data, max?)` (RFC 1952, with the member's CRC-32 and size verified),
  both returning `bytes` and capped at 16 MiB of output (a smaller `max` is
  allowed). The decoder follows Mark Adler's public-domain ("zlib licence")
  puff.c and carries its provenance in the `src/codec.c` header; zlib is not a
  dependency (decision Q17). A truncated, malformed or non-gzip stream and a
  stream past the cap raise `ValueError`.
- Explicit text/binary bridges: `to_string(bytes)` validates UTF-8 and reports the
  offending byte offset (never the payload), `to_bytes(text)`,
  `bytes_to_array(bytes)`, plus the methods `bytes.to_string()` and
  `text.to_bytes()`. `str()` and `print` keep the preview form.
- `to_json` encodes `bytes` as `{"$bytes": "<base64>"}`, a marked object that
  `from_json` returns as a map; `base64_decode(m["$bytes"])` recovers the payload.
- Binary file and socket I/O: `read_bytes(path, max?)` (whole file as bytes, 16 MiB
  cap enforced with a `ValueError`, no NUL truncation), `write_bytes` and
  `append_bytes`, a handle opened with a `b` mode makes `read` return `bytes` and
  `write` accepts bytes as well as text, `send` takes a string or bytes, and
  `recv_bytes(max)` returns bytes. The text forms are unchanged.
- `bytes` has no literal and is never a compile-time constant, so the `.sparkc`
  format keeps its version and constant tags (decision A30); the serializer rejects
  any unknown constant type instead of writing an unreadable file.
- `format(template, args...)`: printf-style text with `%s %d %i %f %g %x %X %o %b %c`
  and `%%`, plus the `-`, `0` and `+` flags, width and `.precision`, built in one
  linear pass.
- Regular expressions: bounded repetition `{n}`, `{n,}` and `{n,m}`, lazy
  quantifiers `*?`, `+?`, `??` and `{n,m}?`, non-capturing groups `(?:...)`, and
  the `i`, `s` and `m` flags as an optional trailing argument or inline as
  `(?i)`, `(?s)`, `(?m)` and `(?-i)`. `regex_match_at(pattern, text, start,
  flags?)` and `regex_find_index(pattern, text, flags?)` expose matches as
  `{start, end}` character offsets, consistent with `len` and `index_of`.
  Matching now uses leftmost-first semantics, and a step budget rejects a
  pathological pattern with `ValueError` instead of hanging the interpreter.
- Capture groups: `regex_match_groups(pattern, text, flags?)` returns
  `{start, end, groups, named}` for the first match, or `nil` when there is none.
  `groups` holds one `{start, end, text}` map per capturing group in
  opening-parenthesis order and `nil` for a group that did not participate;
  `named` keys the same maps by the `(?P<name>...)` name. A group repeated by a
  quantifier keeps its last match, non-capturing groups take no slot, and offsets
  are character offsets. Names must be unique and well formed, and at most 32
  capturing groups are allowed; both are `ValueError`s.
- `regex_split(pattern, text, limit?, flags?)` returns the fields between pattern
  separators, with Perl-like `limit` semantics (maximum field count; `0` returns
  `[]`; the last field keeps the remainder) and empty fields at leading,
  trailing or doubled separators. Zero-width matches are skipped because they
  cannot delimit anything. A string in the third position is read as flags.
- `regex_replace` templates expand capture references: `$0` for the whole match,
  `$1`…`$32`, `${name}` for named groups and `$$` for a literal `$`. A group
  that did not participate expands to nothing, and an unknown, unterminated or
  empty reference is a `ValueError` validated before matching.
- `examples/regex_tokenizer.spark` splits a line, recovers character offsets and
  classifies each field with capture groups; the suite runs it and checks that
  `docs/language.md` carries the same program.
- Math natives: `tan`, `asin`, `acos`, `atan`, `atan2`, `log2`, `log10`, `cbrt`,
  `hypot`, `trunc`, `sign`, `clamp`, `gcd` and `lcm`, plus the `inf` constant.
  Exact operations keep an integer (including `round(x, digits?)`, which returns
  an integer input unchanged); transcendental ones always return a float. Domain
  errors (`asin`/`acos` outside `[-1, 1]`, logs of non-positive values, `nan` in
  `sign`/`clamp`, inverted `clamp` bounds, non-integer `gcd`/`lcm` arguments and
  an `lcm` result past 64 bits) are `ValueError`s. `round(x, digits)` scales in
  binary floating point rather than decimal, which the reference states with
  examples.
- Collection and text conveniences: `count(container, value)` (non-overlapping
  substrings, equal array elements, equal map values, byte values or bytes
  subsequences), `split(text, sep, limit?)` with the same field limit as
  `regex_split`, `trim(text, chars?)` stripping a set of whole UTF-8 characters,
  and `range(start, end, step)` with a non-zero, possibly negative step.
- `sort(array, comparator?)` accepts the `sort_by` comparator (the two are the
  same stable merge sort; `sort_by` stays as an alias), and `shuffle(array)` and
  `choice(array)` draw from the same PRNG as `rand`, so `srand` reproduces them.
- Date and time (`src/datetime.c`): `time_format(timestamp?, format, local?)` and
  `time_parse(text, format, local?)` with `%Y %m %d %H %M %S %j %w %a %A %b %B %z %Z`
  and `%%`, plus `time_utc`/`time_local` offsets and `time_ms()`. UTC is the default
  and the calendar arithmetic is locale-independent; local time is opt-in.
- Cryptography (`src/crypto.c`, through OpenSSL): `sha256`/`sha256_hex` over `bytes`
  or a string, `hmac_sha256(key, data)`, and `rand_bytes(n)` from the operating
  system's CSPRNG (capped at 16 MiB). `rand()` stays the fast, seedable,
  non-cryptographic PRNG. A CSPRNG failure raises instead of falling back.
- The HTTP client advertises `Accept-Encoding: gzip, deflate` and decodes a
  compressed response transparently: `gzip`/`x-gzip` and both the zlib and the raw
  form of `deflate` (told apart by the stream's header), so `body` is always the
  plain bytes. The `max_body` cap is applied to the decompressed body, so a
  compression bomb raises `NetworkError` instead of exhausting memory. A
  caller-supplied `Accept-Encoding` header replaces the default (the header is never
  sent twice), and an encoding the client cannot decode (`br`, say) is a
  `NetworkError` rather than compressed bytes leaking into the body. The decoder is
  the codec module's, so the two paths cannot drift.

### Security

- Network error messages redact URL userinfo (`https://***@host/`) and never echo
  header values or bodies, so a token passed through a URL or an `Authorization`
  header cannot leak through an exception. The embedder trust model (Spark is not a
  sandbox; cap descriptors, sandbox the process, filter egress) is documented in
  `docs/language.md` and `docs/architecture.md`.
- New fixtures cover the HTTP header-count and chunked-body limits and assert that
  URL and header secrets stay out of error messages.
- The crypto natives name the received type in a `TypeError` but never echo the key
  or the data, so a secret cannot leak through an exception; a fixture checks both
  argument positions.
- `inflate`/`gunzip` enforce a 16 MiB output cap, so a decompression bomb raises
  `ValueError` instead of exhausting memory. Fixtures cover a 17 MiB bomb, a
  corrupt member, non-gzip input and a truncation loop over every prefix of a gzip
  and a raw stream.

### Dependencies

- Spark now links **OpenSSL** (`libssl`/`libcrypto`) for the TLS transport and for
  the SHA-256, HMAC-SHA256 and CSPRNG natives in `src/crypto.c`. This is the single
  approved exception to the no-dependencies rule (decision Q11); the socket, HTTP
  and WebSocket layers stay dependency-free. Building requires the OpenSSL
  development headers (`libssl-dev` on Debian/Ubuntu).

### Fixed

- A top-level function that shadows a native keeps tail-call optimization. The
  compiler used to decide from the name alone, so `fn count(n)` — now that
  `count` is a native — recursed with a full frame per call and failed at
  `count(100000, 0)`. It now tracks the functions the program itself declares
  and treats those names as ordinary calls again; the native stays reachable
  through another name.

- The collector no longer loses objects. `object_coroutine` and `object_channel`
  zeroed the whole object after `allocate_obj` had already linked it, which
  cleared `Obj.next` and truncated the GC object list. Live objects could then
  escape marking (a channel kept a stale mark, so the blocked coroutines in its
  wait queues were swept) and blocked coroutines were freed while still queued on
  a channel. `tests/cases/coroutine_gc.spark` covers the regression.
- The language reference no longer contradicts the interpreter: negative indexes
  count from the end, `slice` and the native of the same name clamp instead of
  raising, `substr` accepts a negative `start` but not a negative `count`, and the
  out-of-range example `"spark"[5]` was replaced. The `is_a` row now says that a
  type-name string is rejected. The operator table lists every operator the lexer
  has (`**`, `?.`, `??=`, `++`/`--`, slicing, `in`, `instanceof`), and short lambdas
  plus indexing and slicing are documented.
- The remaining Spanish identifiers, comments, messages and data in the benchmark
  sources and the test fixtures were translated, so every artifact is in English,
  and the rule now lives in `AGENTS.md` as well as `README.md`.
- An out-of-range negative index reports the index the program wrote instead of the
  position it resolved to: `[1, 2, 3][-4]` now says `index out of range: -4 (size 3)`
  rather than `-1`. Positive indexes are unchanged. The same fix covers indexing a
  string and assigning or incrementing through an array index.
- Error messages that name a value type no longer pick a fixed article, which produced
  `tried to update a instance`. They now say `a value of type instance`, and the index
  type error says `received array` instead of `received a array`.
- A server that closes the connection before it answers now reports
  `NetworkError: the HTTP response ended before the status line` (or `... inside the
  headers`). The two branches were unreachable, so both cases returned with no message
  filled in and the error could show whatever the stack happened to hold.
- The HTTP client no longer reuses a connection that was authenticated with a different
  `ca_file`: the persistent-connection pool is keyed by origin **and** trust store, so a
  request for the system store can never ride a connection verified against an injected
  CA.

### Breaking changes

- The HTTP client keeps its connections alive instead of sending `Connection: close`
  with every request. A server that counted one connection per call, or a test that
  asserted a fresh connection each time, now sees a single reused connection; a
  program that needs the old behavior sets `Connection: close` in the `headers`
  argument. Idle connections are closed after 30 s and when the interpreter exits.
  Reusing a connection is also refused when the request asks for a different
  `ca_file` than the one the connection was verified against.
- Regular expressions use leftmost-first semantics (decision Q27). A pattern
  such as `a|ab` now matches `"a"` in `"ab"` where the previous engine returned
  the longest match `"ab"`, and `regex_find("<.*>", "<a><b>")` returns
  `["<a><b>"]` while `<.*?>` returns `["<a>", "<b>"]`. The existing fixtures did
  not encode the old longest-match tie-break, but any program that relied on it
  should be reviewed.
- `^` and `$` now anchor at the start and end of the text only. A pattern that
  should also match at a line boundary needs the `m` flag (`regex_match(p, t,
  "m")` or inline `(?m)`); previously both always saw `\n`.
- `yield` is a reserved keyword, so programs using `yield` as an identifier must
  rename it.
- Class names may no longer be `coroutine` or `channel`, which join the existing
  reserved type names.
- Class names may no longer be `socket` or `server`, which are now reserved type
  names.
- Class names may no longer be `websocket`, which is now a reserved type name.
- The bytecode file format version is now `5` (needed for `OP_YIELD`).
  `.sparkc` files compiled by an earlier build are rejected with exit code 65;
  recompile them with `spark --compile`.
- `print` no longer flushes standard output. It is line-buffered on a terminal
  and block-buffered when redirected or piped, so the bytes are the same but they
  are no longer visible before the program ends or blocks. A program that printed
  a readiness line, a prompt or a progress line and then waited on a pipe, a
  socket or stdin must call `flush()` — or use `input()`, which flushes before it
  reads, where that is the wait. The `examples/` servers and the suite's servers
  were updated this way. Removing the flush drops one `write(2)` per `print`:
  100,000 lines go from 100,003 writes and 33.6 ms to 169 writes and 13.8 ms, and
  1,000,000 `print` calls from 306 ms to 106 ms.

## [0.1.0] - 2026-10-27

Initial release of the interpreter.

### Added

- The language core: `nil`, `bool`, 64-bit integers, floats, strings, arrays,
  maps, closures, classes and instances.
- Control flow (`while`, `do/while`, `for`, `for..in`, labels, `switch`), string
  interpolation, ternary and `??`, bitwise operators, slices and destructuring.
- Functions with default parameters, varargs and spread, tail-call optimization
  and higher-order natives.
- `import` with a module cache, `try`/`catch`/`finally`/`throw`, and a structured
  error map (`type`, `message`, `line`, `column`) that also catches runtime errors.
- Classes with `new`, fields, methods, `extends`, `super`, `static`,
  `instanceof`, `to_string` and operator overloading.
- Native standard library: collections and strings, math, time, random, text,
  files and file handles, `argv`/`env`/`exit`, JSON, regex and string builders.
- Tooling: `--dump`, `--compile` (`.sparkc` bytecode), `--debug` (breakpoints,
  stepping, locals), `--format` and `--lint`, plus a bytecode disassembler.
- A mark-and-sweep collector with adaptive thresholds and an explicit `gc()`.

### Breaking changes

- `+` no longer coerces between numbers and strings: both operands must be two
  numbers or two strings. The error suggests string interpolation or `str(x)`.
- `new` is mandatory for construction: `Foo()` is now an error instead of
  creating an instance.
- Array and string indexes accept negative values, which count from the end. A
  simple out-of-range index is an error, while slices clamp to the valid range.
- `?.` guards a whole call chain, and calling a field that is not a function now
  reports a different error from reading a field that does not exist.
- `type(instance)` returns the class name, class names may not collide with
  built-in type names, and `is_a(x, Class)` respects inheritance.
