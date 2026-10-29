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

### Breaking changes

- `yield` is a reserved keyword, so programs using `yield` as an identifier must
  rename it.
- Class names may no longer be `coroutine` or `channel`, which join the existing
  reserved type names.
- Class names may no longer be `socket` or `server`, which are now reserved type
  names.
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
