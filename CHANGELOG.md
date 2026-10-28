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
- Networking: `tcp_connect`, `tcp_listen`, `udp_bind` and `net_hostname`, plus
  `socket` and `server` GC-managed resource objects. TCP clients and servers and
  UDP datagrams, with timeouts, IPv4/IPv6 resolution (bracketed IPv6 literals
  included) and categorized `NetworkError`/`TimeoutError` failures.

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
