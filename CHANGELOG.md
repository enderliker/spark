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
  bodies, `Content-Length`, read-to-EOF, redirects and the `Connection: close`
  framing, within explicit header (64 KiB), header-count, redirect (5), body
  (16 MiB) and timeout (30 s) limits.
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
- Date and time (`src/datetime.c`): `time_format(timestamp?, format, local?)` and
  `time_parse(text, format, local?)` with `%Y %m %d %H %M %S %j %w %a %A %b %B %z %Z`
  and `%%`, plus `time_utc`/`time_local` offsets and `time_ms()`. UTC is the default
  and the calendar arithmetic is locale-independent; local time is opt-in.

### Security

- Network error messages redact URL userinfo (`https://***@host/`) and never echo
  header values or bodies, so a token passed through a URL or an `Authorization`
  header cannot leak through an exception. The embedder trust model (Spark is not a
  sandbox; cap descriptors, sandbox the process, filter egress) is documented in
  `docs/language.md` and `docs/architecture.md`.
- New fixtures cover the HTTP header-count and chunked-body limits and assert that
  URL and header secrets stay out of error messages.

### Dependencies

- Spark now links **OpenSSL** (`libssl`/`libcrypto`) for the TLS transport only.
  This is the single approved exception to the no-dependencies rule (decision Q11);
  the socket, HTTP and WebSocket layers stay dependency-free. Building TLS requires
  the OpenSSL development headers (`libssl-dev` on Debian/Ubuntu).

### Fixed

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

### Breaking changes

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
