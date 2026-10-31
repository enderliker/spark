# Spark

**Spark** is an interpreted, dynamically typed programming language implemented
from scratch in C11. Source code is parsed into an AST, compiled to a compact
bytecode, and executed on a stack-based virtual machine with a mark-and-sweep
garbage collector. The lexer, parser, compiler, VM, standard library and tooling
are all handwritten; the only third-party dependency is OpenSSL, linked
exclusively for the TLS transport.

[![CI](https://github.com/enderliker/spark/actions/workflows/ci.yml/badge.svg)](https://github.com/enderliker/spark/actions/workflows/ci.yml)
![C11](https://img.shields.io/badge/C-C11-555.svg)
![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Version](https://img.shields.io/badge/version-0.1.0-informational.svg)

> **Status:** 0.1.0 (pre-1.0). The language, standard library and toolchain are
> usable and exercised by a full test suite. The roadmap and its current status
> live in [`tasks/plan.md`](tasks/plan.md).

```spark
fn fib(n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

for (var i = 0; i < 10; i += 1) {
    print("fib(${i}) =", fib(i));
}
```

## Table of contents

- [Features](#features)
- [Requirements and build](#requirements-and-build)
- [Usage](#usage)
- [Quick language guide](#quick-language-guide)
- [Native functions](#native-functions)
- [Project layout](#project-layout)
- [How it works](#how-it-works)
- [Testing and continuous integration](#testing-and-continuous-integration)
- [Benchmarking](#benchmarking)
- [Known limits of this version](#known-limits-of-this-version)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

## Features

### Language

- Types: `nil`, `bool`, `int`, `number`, `string`, `array`, `map`, `function`,
  `class`/`instance`, `string_builder`, `coroutine` and `channel`.
- 64-bit integers alongside doubles: literals, promotion, `div`, `%`, bitwise
  operators (`&`, `|`, `^`, `~`, `<<`, `>>`) and `int`/`float`/`is_int`/`is_nan`.
- Expressions: interpolation (`"${expr}"`), ternary, `??`, power `**`, optional
  chaining `?.`, binary `in`, `++`/`--`, compound assignment, unary `+`/`-`.
- Literals: decimal, hexadecimal, binary and octal numbers with `_` separators,
  multi-line `"""strings"""`, and `\uXXXX` escapes.
- Variables with block-lexical scope, `const` bindings and `var [a, b] = array` /
  `var {x, y} = map` destructuring.
- Control flow: `if`/`else`, `while`, `do`/`while`, C-style `for`, `for-in` over
  arrays, maps and strings (including `for (var i, value in array)`), labelled
  `break`/`continue`, and `switch`/`case`/`default` with auto-break.
- Collections: array and map literals, indexing with negative indexes, slices
  (`a[1:4]`, `a[::-1]`), and a full set of array/map natives.
- Functions: first-class values, recursion, closures captured by reference,
  default parameters, varargs (`fn f(...rest)`) and spread calls (`f(...args)`),
  short lambdas (`fn(x) => x * x`) and tail-call optimization.
- Classes: fields, methods, `init`, `new`, single inheritance with `extends` and
  `super`, `static` members, `instanceof`, `to_string()` and operator overloading
  (`__add__`, `__eq__`, `__index__`, `__len__`, ...).
- Errors: `try`/`catch`/`finally`/`throw` with structured error maps
  (`type`, `message`, `line`, `column`); runtime errors are catchable too.
- Modules: `import` with per-path caching, cycle detection and a shared global
  namespace.
- Concurrency: cooperative **coroutines** (`spawn`, `yield`, `join`, `done`) and
  FIFO **channels** (`channel`, `send`, `receive`, `close`) on a single thread.

### Standard library and runtime

- Text and collection helpers, math, time and randomness, file and directory I/O,
  `exec`, environment variables, script arguments, JSON, regular expressions and a
  string builder.
- Networking: TCP and UDP sockets as GC-managed resources with timeouts, name
  resolution over IPv4 and IPv6, and typed `NetworkError`/`TimeoutError` failures.
  Blocking calls yield only their coroutine, so one process serves many
  connections. URL helpers, an HTTP/1.1 client (chunked bodies, redirects, size and
  timeout limits), TLS with certificate and host-name verification, HTTPS, an HTTP
  server library on the socket layer and an RFC 6455 WebSocket client
  (`ws://`/`wss://`).
- A virtual machine with fused superinstructions, hot/cold dispatch, a growable
  value stack and an adaptive garbage collector.

### Tooling

- Interactive REPL, bytecode disassembler (`--dump`), precompiled bytecode
  (`--compile`), interactive debugger (`--debug`), formatter (`--format`) and
  linter (`--lint`).

## Requirements and build

The build requires a C11 compiler, `make`, the C math library and the OpenSSL
development files (`libssl-dev` on Debian/Ubuntu), which are used only by
`tls_connect` and `https://`. OpenSSL is the sole third-party dependency;
`python3` is needed to run the test suite and the benchmarks.

```bash
make            # builds the ./spark binary
make test       # runs the test suite (needs python3)
make coverage   # runs tests and reports gcov code coverage
```

Development builds with sanitizers:

```bash
make clean
make CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic" \
     LDFLAGS="-fsanitize=address,undefined"
python3 tests/run_tests.py
```

On **Windows** the recommended path is WSL (Ubuntu): open a WSL terminal, enter
the project directory and run `make` and `./spark`. The system natives (`exec`,
`list_dir`, `mkdir`, `sleep`) use POSIX APIs, so Linux, macOS and WSL are the
supported hosts.

## Usage

```bash
./spark                              # opens the REPL
./spark examples/fibonacci.spark
./spark program.sparkc                # runs precompiled bytecode
./spark --dump file.spark             # shows the bytecode without running
./spark --compile file.spark          # compiles source to .sparkc (-o for the output path)
./spark --debug file.spark            # runs file under the interactive debugger
./spark --format file.spark           # outputs canonical formatted code to stdout
./spark --format -w file.spark        # formats file in-place
./spark --lint file.spark             # reports unused variables, unreachable code and shadowing
./spark --help
./spark --version
```

The interpreter returns `0` on success, `64` for incorrect CLI usage, `65` for a
syntax or compilation error, `70` for a runtime error and `74` if it cannot read
the file. The linter returns `1` when it reports findings.

In the REPL every standalone expression prints its value, global variables persist
between lines and the last result is kept in `_`:

```
spark> 1 + 2
3
spark> var greeting = "hello"
spark> greeting + ", world"
hello, world
spark> exit
```

The examples under [`examples/`](examples) are small, runnable programs covering
closures, collections, sorting and networking. [`http_client.spark`](examples/http_client.spark)
fetches a URL, [`http_server.spark`](examples/http_server.spark) serves one with
[`lib/http.spark`](lib/http.spark), [`echo_server.spark`](examples/echo_server.spark)
echoes lines and [`discord_bot.spark`](examples/discord_bot.spark) speaks the
Discord gateway protocol against a local mock gateway.

## Quick language guide

```spark
// Line comment
/* Block
   comment */

var name = "world";
print("hello, ${name}!");

var numbers = [3, 1, 2];
numbers[0] = 30;
push(numbers, 4);
print(numbers, len(numbers), numbers[-1]);

var [first, second] = numbers;
print(first, second, numbers[1:3]);

var person = {"name": "Ada", "age": 36};
person["age"] += 1;
print(person["name"], has(person, "age"));

fn add(a, b = 10) {
    return a + b;
}
print(add(1), add(1, 2));

var square = fn(x) => x * x;
print(map([1, 2, 3], square), sort([3, 1, 2]));

var total = 0;
for (var i in range(1, 6)) {
    total += i;
}
while (total > 10) {
    total -= 10;
}
print(total, type(total), str(total));

class Point {
    var x = 0;
    var y = 0;

    init(x, y) {
        self.x = x;
        self.y = y;
    }

    length() {
        return sqrt(self.x * self.x + self.y * self.y);
    }

    to_string() {
        return "(${self.x}, ${self.y})";
    }
}

var point = new Point(3, 4);
print(point, point.length());

try {
    throw {"message": "boom"};
} catch (e) {
    print("caught", e.message);
}

fn worker(label) {
    yield;
    return "${label} done";
}

var builder = string_builder();
builder.append("built: ");
builder.append(1 + 1);
print(builder.to_string());

var job = spawn(worker, "job");
print(job.join());
```

The full reference is in [`docs/language.md`](docs/language.md) and the internals
are in [`docs/architecture.md`](docs/architecture.md).

## Native functions

**Values, text and types**

| Function | Description |
| --- | --- |
| `print(...)` | Prints its arguments separated by spaces and a newline. |
| `str(x)` | Text representing the value (uses `to_string()` on instances). |
| `num(x)` | Number from a numeric string (always a float); numbers pass through. |
| `int(x)` / `float(x)` | Convert between integers and floats (or from a numeric string). |
| `is_int(x)` | Whether the value is an integer. |
| `is_nan(x)` | Whether the value is IEEE 754 NaN. |
| `type(x)` | Type name; for an instance it is the class name. |
| `len(x)` | Length of a string (UTF-8 characters), array or map; uses `__len__` on instances. |
| `assert(cond, message?)` | Stops the program with an error when `cond` is false. |
| `copy(array_or_map)` | Shallow copy; any other value is returned unchanged. |
| `isinstance(x, Class)` / `is_a(x, Class)` | Whether `x` is an instance of the class or a subclass. |
| `gc()` | Runs a garbage collection cycle immediately. |
| `clock()` | Monotonic clock seconds, for measuring. |
| `time()` | Wall-clock seconds since the Unix epoch. |
| `sleep(ms)` | Suspends the program for the given number of milliseconds. |

**Numbers**

| Function | Description |
| --- | --- |
| `abs(x)` | Absolute value; an integer stays an integer. |
| `floor(x)` / `ceil(x)` / `round(x)` | Rounding (integers are returned unchanged). |
| `sqrt(x)` | Square root (error on negative input). |
| `pow(x, y)` | `x` raised to `y`. |
| `sin(x)` / `cos(x)` / `log(x)` / `exp(x)` | Trigonometry (radians), natural logarithm and exponential. |
| `min(a, b, ...)` / `max(a, b, ...)` | Smallest/largest of the arguments, or of an array when given one array. |
| `sum(array)` | Sum of the numeric elements (integer when all are integers). |

**Strings**

| Function | Description |
| --- | --- |
| `substr(text, start, count?)` | Substring by UTF-8 characters. |
| `split(text, sep)` / `join(array, sep)` | Text to array and back (empty separator splits into characters). |
| `trim(text)` / `upper(text)` / `lower(text)` | ASCII whitespace trim and ASCII case conversion. |
| `repeat(text, n)` / `replace(text, old, new)` | Repeat and replace all occurrences. |
| `starts_with(text, prefix)` / `ends_with(text, suffix)` | Prefix/suffix test. |
| `contains(container, value)` | Membership in an array, substring in a string or key in a map. |
| `index_of(container, value)` | First position (elements or UTF-8 characters) or `-1`. |
| `chr(code)` / `ord(text)` | Unicode codepoint to one-character string and back. |
| `pad_start(text, width, fill?)` / `pad_end(...)` | Pads to `width` characters (fill defaults to a space). |

**Arrays and maps**

| Function | Description |
| --- | --- |
| `push(array, value)` / `pop(array)` | Append, and remove and return the last element. |
| `insert(array, index, value)` / `remove(array, index)` | Insert at `index`, and remove and return the element at `index`. |
| `reverse(array)` / `fill(array, value)` | Reverse / overwrite in place; return the array. |
| `slice(array, start, end?)` | New array with `[start, end)`. |
| `range(n)` / `range(start, end)` | Array of integers in `[start, end)`. |
| `has(map, key)` / `del(map, key)` | Whether the map contains the key, and delete it. |
| `keys(map)` / `values(map)` | Arrays with the keys / values. |
| `clear(map)` / `merge(dest, src)` / `get(map, key, default?)` | Map helpers. |
| `sort(array)` / `sort_by(array, fn)` | Stable sort in place (numbers or strings) with an optional comparator. |

**Callbacks**

| Function | Description |
| --- | --- |
| `map(array, fn)` / `filter(array, fn)` | Transform / select with a callback. |
| `reduce(array, fn, initial?)` / `each(array, fn)` | Fold and iterate with a callback. |
| `any(array, fn?)` / `all(array, fn?)` | Whether any/all elements are truthy, or satisfy `fn(x)`. |
| `enumerate(array)` | Array of `[index, value]` pairs. |
| `zip(a, b)` | Array of `[a[i], b[i]]` pairs, up to the shorter array. |

**Files, system and data**

| Function | Description |
| --- | --- |
| `input(prompt?)` | Reads a line from standard input; returns `nil` on EOF. |
| `read_file(path)` / `write_file(path, data)` / `append_file(path, data)` | Whole-file helpers. |
| `file_exists(path)` | Whether the path exists. |
| `open(path, mode?)` | Opens a file handle (default mode `"r"`). |
| `read_line(file)` / `read(file, bytes?)` | Reads a line / up to `bytes` from a handle. |
| `write(file, data)` / `close(file)` / `eof(file)` | Writes, closes and tests a handle. |
| `list_dir(path)` / `mkdir(path)` | Directory listing and creation. |
| `exec(command)` | Runs a shell command; returns a map with `exit_code`, `stdout` and `stderr`. |
| `env(name?)` / `exit(code?)` | Environment variables and process exit. |
| `argv` | Global array with the script path and its arguments (empty in the REPL). |
| `rand()` / `randint(min, max)` / `srand(seed)` | PRNG in `[0, 1)`, random integers and seeding. |
| `to_json(value)` / `from_json(text)` | JSON encoding and decoding. |
| `regex_match(p, t)` / `regex_find(p, t)` / `regex_replace(p, r, t)` | Regular expressions. |
| `string_builder(initial?)` / `sb_append(sb, v)` | Build a large string in amortized O(1) per append. |
| `sb_to_string(sb)` / `sb_len(sb)` / `sb_clear(sb)` | Builder text, character length and reset. |
| `spawn(fn, args...)` | Starts a coroutine; returns the coroutine object. |
| `channel(n?)` | Creates a channel: unbuffered without `n`, buffered with it. |

The global constants `pi` and `nan` are predefined, and builders also accept the
method form (`sb.append(v)`, `sb.to_string()`, `sb.len()`, `sb.clear()`).
[`docs/language.md`](docs/language.md) documents the edge cases and error behavior.

### Networking

| Function | Description |
| --- | --- |
| `tcp_connect(host, port, timeout_ms?)` | Connects a TCP client; returns a `socket`. |
| `tls_connect(host, port, timeout_ms?, ca_file?)` | Connects a TLS client; returns a `socket` with `read`/`write`. |
| `ws_connect(url, timeout_ms?, ca_file?)` | Opens a WebSocket client; returns a `websocket`. |
| `tcp_listen(host, port, backlog?)` | Starts a TCP listener; returns a `server`. |
| `udp_bind(host, port, timeout_ms?)` | Binds a UDP socket; returns a `socket`. |
| `net_select(readables, timeout_ms?)` | Waits for any socket to be readable; returns the ready indexes. |
| `net_hostname()` | The local host name. |
| `url_parse(url)` | Splits a URL into a `{scheme, host, port, path, query, fragment}` map. |
| `url_encode(text)` / `url_decode(text)` | Percent-encodes / decodes form data. |
| `parse_query(query)` | Parses a query string into a map from key to array of values. |
| `http_get` / `http_post` / `http_put` / `http_delete` | Sends an HTTP/1.1 request; returns `{status, headers, body}`. |
| `http_request(options)` | The map-driven form of the HTTP client. |

Socket methods: `sock.send`, `sock.recv`, `sock.recv_line`, `sock.wait`, `sock.send_to`,
`sock.recv_from`, `sock.peer`, `sock.local` and `sock.close`. TLS sockets use
`sock.read` and `sock.write` instead of the TCP methods. Server methods:
`srv.accept`, `srv.port` and `srv.close`. WebSocket methods: `ws.send_text`,
`ws.send_binary`, `ws.receive`, `ws.ping` and `ws.close`; `receive` returns
`{opcode, data}` or `nil` and reassembles fragmented messages.

Hosts may be names, IPv4 literals or bracketed IPv6 literals; `localhost` works
over either family. A blocking call suspends its coroutine and the scheduler polls
the descriptors, so a single process serves many connections with `spawn`. TLS and
`wss://` verify the certificate chain and the host name by default (system store or
an explicit `ca_file`) and have no way to disable verification. The HTTP client
follows redirects and handles chunked bodies within explicit header, body, redirect
and timeout limits; `http_request` accepts a `ca_file` option for a private CA, and
`lib/http.spark` builds an HTTP server on top of the socket layer with no extra
native code. [`examples/discord_bot.spark`](examples/discord_bot.spark) shows a
WebSocket client with heartbeats and a local mock gateway in the test suite.

## Project layout

```
src/
  main.c                      CLI, REPL and sample drivers
  lexer.c                     lexical analysis
  parser.c                    syntax analysis and AST construction
  ast.c                       AST memory arena
  compiler.c                  AST -> bytecode (slots, upvalues, folding, TCO)
  chunk.c                     bytecode blocks, constants and disassembler
  vm.c                        virtual machine, GC, natives, classes, coroutines
  net.c                       sockets, TLS, URLs, HTTP and WebSocket clients
  object.c                    dynamic values, interned strings, arrays, maps
  bytecode.c                  .sparkc serialization
  json.c / regex.c            JSON and regular-expression engines
  debugger.c                  interactive debugger
  formatter.c / linter.c      developer tools
  table.c                     hash table of global names (name -> slot)
  memory.c                    memory accounting and GC trigger
  error.c                     diagnostics and source caret
  buf.c                       auxiliary dynamic buffer
  value.h                     value representation
lib/                          standard library written in Spark
docs/                         language reference and architecture notes
examples/                     example programs
tests/                        automated tests and fixtures
benchmarks/                   microbenchmarks and runners
```

## How it works

1. **Lexer**: turns the text into tokens with a line number.
2. **Parser**: builds an AST with recursive descent and precedences; nodes are
   allocated in an arena that is freed all at once when compilation finishes.
3. **Compiler**: walks the AST and emits bytecode with local slots, upvalue
   resolution at compile time, constant folding, dead-code elimination, jump
   patching and tail calls.
4. **VM**: executes the bytecode on a value stack with call frames, closures,
   natives and a *mark & sweep* GC. The hot loop keeps the instruction pointer and
   stack pointer in registers, uses computed-goto dispatch and fuses common
   instruction sequences (local/constant arithmetic, comparison-and-branch,
   direct local returns) into superinstructions.

The bytecode uses a 16-bit constant index and a line entry per instruction. For
example, `print(1 + 2);` folds at compile time into:

```
$ ./spark --dump hello.spark
== hello.spark ==
0000    1 GET_GLOBAL            0 'print'
0003    | CONSTANT              0 '3'
0006    | CALL                  1
0008    | POP
0009    | NIL
0010    | RETURN
```

## Testing and continuous integration

`tests/run_tests.py` runs valid programs comparing exact output (`tests/cases`),
error cases checking the exit code and a `stderr` fragment (`tests/errors`), REPL
sessions and the CLI options, including the debugger, formatter, linter and
precompiled bytecode. The current suite contains 239 tests and all of them pass.

CI (`.github/workflows/ci.yml`) runs four jobs on every push to `main` and on every
pull request:

- **Build and test** with `-Werror` and then the full suite.
- **Sanitizers** (`-fsanitize=address,undefined`) and the full suite.
- **Benchmark**: a coarse `fib(27)` regression guard (`--max-ms 100` on shared
  runners).
- **Coverage**: a `--coverage` build, the suite, and a `gcov` report.

Test fixtures never touch the public internet: networking tests run local servers
and TLS servers on ephemeral ports.

## Benchmarking

`benchmarks/run.py` runs `benchmarks/fib.spark` (fib(27)) several times and reports
the minimum and median; the minimum is around 4.9 ms on an idle development
machine, and the performance gate is a minimum below 5 ms.
`benchmarks/run_all.py` runs the wider set (loops, closures, arrays, maps, sorting,
strings, allocation, TCP echo and HTTP round-trips) and prints min/median per
benchmark. The network benchmarks stay offline: `echo.spark` uses an in-process
coroutine server, while `http_roundtrip.spark` talks to a loopback server that
`run_all.py` starts in another process (the HTTP client blocks the process rather
than yielding its coroutine).

`tests/cases/string_builder_scale.spark` guards the amortized behavior of
`string_builder`: it measures the same loop at two sizes and fails when quadrupling
the work multiplies the time by more than about 8x. `s += x` in a loop is
deliberately quadratic (strings are immutable values); use the builder to assemble
large strings.

## Known limits of this version

- Coroutines are cooperative and single-threaded: there are no OS threads or
  parallelism. `yield` and a blocking `send`/`receive`/`join` are rejected inside
  native callbacks and imported modules, and so is a would-block socket call.
- Modules share one global namespace; there are no per-module namespaces.
- Assignments are statements (`x = 1;`), not expressions.
- Strings are immutable, so `s += x` in a loop is O(n²); use `string_builder`.
- Closures capture the *variable*, not a copy. Variables declared in a loop body
  are fresh per iteration, but the loop variable itself is shared across
  iterations.
- Integers are 64-bit and floats are IEEE doubles; mixed arithmetic and integer
  overflow promote to a float.
- Size limits: 255 parameters and arguments per call, 255 elements per array
  literal, 1024 locals per function, 65,536 constants per function, 4096 call
  frames (tail calls reuse frames) and a value stack that grows from 2,048 to
  4,194,304 entries. Channels hold up to 16,777,216 values. See
  [`docs/language.md`](docs/language.md) section 19.
- Sockets are non-blocking under the hood: a call that would block suspends its
  coroutine, and the scheduler sleeps in `poll(2)` until a descriptor is ready or
  the nearest deadline fires.
- Spark is not a sandbox: `exec` and the network natives are available to any
  program, so untrusted code should not be run directly.

## Roadmap

A native code generator (JIT) instead of the bytecode interpreter, OS threads on
top of the existing coroutines, a richer standard library, and namespaced modules
are candidates for future work. The ordered roadmap and its current status are
tracked in [`tasks/plan.md`](tasks/plan.md); released and unreleased changes are in
[`CHANGELOG.md`](CHANGELOG.md).

## Contributing

Contributions are welcome. Before opening a change:

1. Build with `make` (the build must be warning-free) and run
   `python3 tests/run_tests.py`.
2. Add or update a fixture for any new behavior: `tests/cases` for valid programs
   compared against exact `.out` output, `tests/errors` for failing programs with
   `exit:`/`stderr:` specifications.
3. Measure VM changes with `python3 benchmarks/run.py` instead of guessing.
4. Read [`AGENTS.md`](AGENTS.md) for the full conventions and the definition of
   done.

All project artifacts (code, comments, documentation, user-facing messages and
commit messages) are written in English.

## License

Released under the [MIT License](LICENSE).
