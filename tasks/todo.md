# Spark task list

Detailed, dependency-ordered task list for the roadmap in `tasks/plan.md`. Execute
top to bottom; do not start a phase before its checkpoint is approved. Every task
must satisfy the Definition of Done in the plan (warning-free build, tests, docs,
sanitizers for VM tasks, benchmark <= 5 ms).

Legend: **scope** S (1-2 files), M (3-5), L (5-8). Anything XL is split further.

---

## Phases 15-20: Networking

The completed phases 1-14 and tasks T1-T81 were removed to keep this file short; their
record is in git history, `CHANGELOG.md` and `docs/intent/spark-completion.md`. The
plan for this stream is in `tasks/plan.md` (overview, decisions A9-A15, risk table and
open questions); nothing here is implemented yet.

## Phase 15 - Sockets (blocking core, no new dependencies)

### T82 - `ObjSocket`/`ObjServer`: resource objects and the `net` module skeleton
**Description:** Add `OBJ_SOCKET` and `OBJ_SERVER` to the object model following
`ObjFile`: they own a file descriptor and an `is_closed` flag, are closed by
`close()`, by the collector's finalizer and on VM shutdown, and report `"socket"` /
`"server"` from `type()`. New `src/net.c` / `src/net.h` hold the natives, registered
from `vm_init`. No I/O yet.
**Acceptance criteria:**
- [x] `type(sock)` is `"socket"` / `"server"` and using a closed handle raises a clear runtime error.
- [x] Descriptors do not leak: the finalizer closes an unreferenced socket and `vm_free` closes the rest.
- [x] The build picks the new module up unchanged and stays warning-free.
**Verification:**
- [x] Positive fixture (`tests/cases/`) plus error fixtures for the closed handle; fd-count assertion in the fixture.
- [x] Suite + ASan/UBSan.
**Dependencies:** None.
**Files:** `src/value.h`, `src/object.h`, `src/object.c`, `src/vm.c`, `src/net.c`, `src/net.h`, `tests/`.
**Scope:** M.

### T83 - TCP client (`tcp_connect`, `send`, `recv`, `recv_line`)
**Description:** `tcp_connect(host, port, timeout_ms?)` returns a connected socket;
`sock.send(data)` writes everything and returns the byte count; `sock.recv(max)`
returns up to `max` bytes or `nil` at EOF; `sock.recv_line(max?)` reads up to a
newline (bounded); `sock.close()`, `sock.peer()`, `sock.local()`.
**Acceptance criteria:**
- [x] Round-trip against a local echo server: several sends, a `recv_line`, then EOF as `nil`.
- [x] `timeout_ms` bounds connect and read; exceeding it raises a timeout error instead of hanging.
- [x] Short writes are completed; sending after the peer closed raises a network error.
**Verification:**
- [x] Fixtures driven by a local server; suite; ASan/UBSan.
**Dependencies:** T82.
**Files:** `src/net.c`, `src/net.h`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T84 - TCP server (`tcp_listen`, `accept`)
**Description:** `tcp_listen(host, port, backlog?)` returns a server object;
`srv.accept(timeout_ms?)` returns a connected socket, `nil` on timeout and an error
when the server is closed; `srv.port()` reports the bound port (0 binds an ephemeral
one, which the fixtures rely on); `srv.close()`.
**Acceptance criteria:**
- [x] A Spark server accepts a Spark client and echoes it back; binding port 0 then reading `srv.port()` works.
- [x] Closing the server releases the port and a second listen on the same port succeeds.
- [x] `accept` wakes on a pending connection and returns `nil` on timeout.
**Verification:**
- [x] Fixture with server and client in the same program; suite; ASan/UBSan.
**Dependencies:** T82.
**Files:** `src/net.c`, `src/net.h`, `tests/`, `docs/language.md`.
**Scope:** M.

### T85 - Resolution, IPv4/IPv6, error categories and limits
**Description:** Resolve names with `getaddrinfo` and connect to IPv4 or IPv6;
add `ERROR_NETWORK` and `ERROR_TIMEOUT` to `src/error.h` with distinct messages for
unknown host, refused connection, invalid port and descriptor exhaustion; add
`net_hostname()`.
**Acceptance criteria:**
- [x] Refused, unknown host, invalid port and timeout produce distinct categories and messages (error fixtures).
- [x] `localhost` works over both families and an IPv6 literal (`[::1]`) connects.
- [x] Running out of descriptors raises a clear error rather than crashing.
**Verification:**
- [x] `tests/errors` fixtures for each failure; suite.
**Dependencies:** T82-T84.
**Files:** `src/error.h`, `src/error.c`, `src/net.c`, `src/net.h`, `tests/errors`, `docs/language.md`.
**Scope:** M.

### T86 - UDP (`udp_bind`, `send_to`, `recv_from`)
**Description:** `udp_bind(host, port, timeout_ms?)` returns a datagram socket;
`sock.send_to(data, host, port)`; `sock.recv_from(max)` returns
`{data, host, port}`; `sock.close()`.
**Acceptance criteria:**
- [x] Two sockets exchange a datagram and `recv_from` reports the sender address and port.
- [x] `timeout_ms` bounds `recv_from` and the payload cap is enforced.
**Verification:**
- [x] Fixture; suite; ASan/UBSan.
**Dependencies:** T82.
**Files:** `src/net.c`, `src/net.h`, `tests/`, `docs/language.md`.
**Scope:** S.

## Checkpoint: Phase 15
- [x] Suite green (optimized and ASan/UBSan), `-Werror` build
- [x] A Spark client talks to a Spark server on localhost
- [ ] Benchmark minimum unchanged: `fib(27)` measures ~1-5% higher than the
      pre-change build, but the delta tracks the number/length of registered
      natives (adding four dummy natives to the pre-change build reproduces it),
      so it is heap-layout noise in the benchmark, not a hot-path change. The
      baseline itself measured ~5.1-5.25 ms on this (shared) machine.
- [ ] Review with the user before Phase 16

---

## Phase 16 - Asynchronous I/O on the coroutine scheduler

### T87 - Spike: a socket wait that yields the coroutine
**Description:** Tracer bullet for the highest-risk change. Add readiness wait kinds
to the coroutine state (`CORO_WAIT_READ`/`CORO_WAIT_WRITE`), make `sock.wait(read?,
write?, timeout_ms?)` snapshot the coroutine and return `INTERPRET_BLOCKED`, and make
`vm_scheduler` poll the registered descriptors (starting with `poll(2)`) before
picking the next coroutine, waking on readiness or deadline. Record the spike's
outcome (poll vs epoll, wake path, timeout handling) in `docs/architecture.md`.
**Acceptance criteria:**
- [x] One coroutine waiting on a socket lets another run and wakes as soon as the descriptor is ready.
- [x] A deadline wakes the waiter with a timeout error; an idle wait does not spin (CPU near zero).
- [x] The spike notes and the chosen data structures are recorded in `docs/architecture.md`.
**Verification:**
- [x] Fixture with two coroutines sharing a socket pair; benchmark gate; ASan/UBSan.
**Dependencies:** T83.
**Files:** `src/vm.c`, `src/object.h`, `src/net.c`, `docs/architecture.md`, `tests/`.
**Scope:** M.

### T88 - `net_select` and timer-aware polling
**Description:** Generalise the poll loop: `net_select(readables, timeout_ms)` (an
array of sockets) plus a timeout queue so several deadlines coexist and the scheduler
sleeps exactly until the next one.
**Acceptance criteria:**
- [x] One coroutine services several sockets at once and reports which are ready.
- [x] Several concurrent deadlines fire within a bounded slack (no fixed sleep granularity).
**Verification:**
- [x] Fixture with three sockets and two distinct timeouts; suite; ASan/UBSan.
**Dependencies:** T87.
**Files:** `src/net.c`, `src/vm.c`, `tests/`, `docs/language.md`, `docs/architecture.md`.
**Scope:** M.

### T89 - Coroutine-aware `accept`/`recv`/`send`
**Description:** Route the blocking socket operations through the wait machinery so a
would-block operation suspends only its coroutine. Extend the existing `run_depth > 1`
guard (currently used by `yield` and channel waits) to these waits with the documented
message.
**Acceptance criteria:**
- [x] A server coroutine handles two clients concurrently in a single process.
- [x] Calling a would-block operation inside a native callback or an imported module raises the documented error instead of stalling.
- [x] Timeouts and EOF keep the behavior shipped in Phase 15.
**Verification:**
- [x] Fixture with a server and two clients; suite; sanitizers.
**Dependencies:** T87, T88.
**Files:** `src/net.c`, `src/vm.c`, `tests/`, `docs/language.md`.
**Scope:** M.

### T90 - Load fixture and no-busy-wait proof
**Description:** Fixture that runs 32 concurrent echo clients against one server
coroutine, asserting every connection completes, the descriptor count returns to the
baseline and the run does not burn CPU while idle. Add an echo benchmark.
**Acceptance criteria:**
- [x] 32 concurrent connections complete with a single server coroutine and no descriptor leak.
- [x] The benchmark runs `fib(27)` and the echo throughput; the `fib` minimum stays below 5 ms.
**Verification:**
- [x] Fixture; `python3 benchmarks/run.py`; ASan/UBSan.
**Dependencies:** T89.
**Files:** `tests/`, `benchmarks/`, `benchmarks/run_all.py`.
**Scope:** S.

## Checkpoint: Phase 16
- [x] N concurrent connections served by one process with no busy-wait (32 clients, one server coroutine)
- [x] `fib(27)` minimum below 5 ms (4.97 ms); sanitizers clean
- [x] The spike succeeded: `poll(2)` chosen, wake path and data structures recorded in `docs/architecture.md`

---

## Phase 17 - HTTP/1.1 client and server

### T91 - URL parsing and encoding
**Description:** `url_parse(url)` returns a map (`scheme`, `host`, `port`, `path`,
`query`, `fragment`), `url_encode(text)` / `url_decode(text)` handle percent-encoding
and `+`, and `parse_query(query)` returns a map of arrays.
**Acceptance criteria:**
- [ ] Edge cases covered: percent-encoding, `+`, IPv6 literals (`[::1]`), default ports for `http`/`https`, missing path, empty query.
- [ ] Malformed input raises a value error instead of producing a broken map.
**Verification:**
- [ ] Positive and error fixtures; suite.
**Dependencies:** None (pure string work).
**Files:** `src/net.c`, `src/net.h`, `tests/`, `docs/language.md`.
**Scope:** S.

### T92 - HTTP/1.1 client
**Description:** `http_get/post/put/delete(url, body?, headers?, timeout_ms?,
follow_redirects?, max_body?)` (and a `http_request(map)` form) return `{status,
headers, body}`. Handle `Content-Length` and chunked bodies, `Connection: close`,
redirect limits, header-size and body-size caps, and timeouts.
**Acceptance criteria:**
- [ ] Works against a local HTTP server for a plain body, a chunked body and a redirect chain.
- [ ] Caps enforced: too many redirects, oversized header and oversized body fail with explicit errors.
- [ ] Malformed status line and premature EOF raise network errors, not hangs.
**Verification:**
- [ ] Fixtures against a local server that scripts the responses; suite; ASan/UBSan.
**Dependencies:** T83, T85.
**Files:** `src/net.c`, `src/net.h`, `tests/`, `docs/language.md`.
**Scope:** M.

### T93 - HTTP server as a Spark library
**Description:** Build the server on the socket layer as a Spark library plus an
example, keeping the native surface small; request parsing (method, target, headers,
body) and a handler callback.
**Acceptance criteria:**
- [ ] The example answers a real request (driven by Spark's own client) with the right status, headers and body.
- [ ] Parsing limits and errors are documented and covered.
**Verification:**
- [ ] Fixture: Spark server + Spark client; suite.
**Dependencies:** T84, T92.
**Files:** `examples/`, `tests/`, `docs/language.md`.
**Scope:** S.

## Checkpoint: Phase 17
- [ ] A Spark program fetches a local endpoint and a Spark server answers a real client
- [ ] Body, header, redirect and timeout limits enforced and covered by error fixtures

---

## Phase 18 - TLS (decision-gated on Q11)

### T94 - TLS transport with certificate verification
**Description:** `tls_connect(host, port, timeout_ms?, ca_file?)` plus `read`, `write`
and `close`, doing the handshake over the socket layer with SNI and verifying the
chain against the system store (or `ca_file`) and the hostname. Only reachable if Q11
approves the dependency (or the tunnel alternative is chosen instead).
**Acceptance criteria:**
- [ ] Connects to a local TLS server with the fixture CA and exchanges data both ways.
- [ ] A self-signed certificate, a wrong hostname and an untrusted chain are all rejected with a clear error.
- [ ] There is no silent way to skip verification; the trust model is documented.
**Verification:**
- [ ] Fixtures against a local TLS server; suite; ASan/UBSan; security checklist review.
**Dependencies:** T83, Q11, Q14.
**Files:** `Makefile`, `src/net.c`, `src/net.h`, `tests/`, `docs/language.md`, `AGENTS.md`.
**Scope:** M.

### T95 - HTTPS through the HTTP client
**Description:** Let the HTTP client use the TLS transport for `https://` URLs,
including the verification failures and the CA injection used by tests.
**Acceptance criteria:**
- [ ] `https://` against the local TLS server returns the scripted response.
- [ ] Verification failures surface the same errors as T94 and are covered by fixtures.
**Verification:**
- [ ] Fixtures; suite; sanitizers.
**Dependencies:** T92, T94.
**Files:** `src/net.c`, `tests/`, `docs/language.md`.
**Scope:** S.

## Checkpoint: Phase 18
- [ ] Self-signed and mismatched-host certificates are rejected and covered by tests
- [ ] The dependency decision is recorded in `AGENTS.md` and `CHANGELOG.md`

---

## Phase 19 - WebSocket (enables the Discord gateway)

### T96 - WebSocket client
**Description:** RFC 6455 client over `ws://` (and `wss://` once T95 lands):
upgrade handshake, client masking, text/binary/ping/pong/close opcodes,
fragmentation, a maximum frame size and protocol-error handling. API:
`ws_connect(url, timeout_ms?)`, `ws.send_text/binary(data)`, `ws.receive()` →
`{opcode, data}` (or `nil` when closed), `ws.ping(data?)`, `ws.close(code?, reason?)`.
**Acceptance criteria:**
- [ ] Handshake, text and binary round-trips, ping/pong and the close handshake work against a local server.
- [ ] Fragmented messages are reassembled and oversized frames are rejected.
- [ ] A protocol violation closes with the right code and error.
**Verification:**
- [ ] Fixtures against a local WebSocket server; suite; ASan/UBSan.
**Dependencies:** T83, T85 (and T95 for `wss://`).
**Files:** `src/net.c`, `src/net.h`, `tests/`, `docs/language.md`.
**Scope:** M.

### T97 - WebSocket server (optional)
**Description:** Accept the upgrade on an HTTP server and speak the same framing, so
the client fixtures and examples can run without third-party servers.
**Acceptance criteria:**
- [ ] A Spark server echoes a Spark client, including ping/pong and close.
**Verification:**
- [ ] Fixture; suite.
**Dependencies:** T93, T96.
**Files:** `src/net.c`, `examples/`, `tests/`.
**Scope:** S.

### T98 - End-to-end against a local mock gateway
**Description:** A local mock gateway (test fixture) that speaks the Discord-shaped
handshake (HELLO/IDENTIFY/HEARTBEAT/READY/DISPATCH) plus `examples/discord_bot.spark`
that connects, identifies, heartbeats and answers a message event. Never the real
Discord API in tests.
**Acceptance criteria:**
- [ ] The fixture passes offline and fails if the heartbeat or the close handshake is wrong.
- [ ] The example documents the token via `env` and needs no edits to run against the mock.
**Verification:**
- [ ] Fixture; suite; manual run of the example against the mock printed in the docs.
**Dependencies:** T96.
**Files:** `tests/`, `examples/`, `docs/language.md`.
**Scope:** S.

## Checkpoint: Phase 19
- [ ] The mock-gateway fixture passes offline
- [ ] The Discord example is documented step by step

---

## Phase 20 - Hardening, docs and ecosystem

### T99 - Security hardening pass
**Description:** Apply the security checklist to the whole surface: default timeouts
on every operation, connection and descriptor caps, body/frame/header size limits,
redirect limits, secret hygiene (tokens through `env`, never logged), the SSRF surface
of outbound connections, and a plain statement that `exec` already grants network
access so Spark is not a sandbox.
**Acceptance criteria:**
- [ ] Every limit has a fixture that trips it, and every failure is a categorized error.
- [ ] No secret can be echoed by a network error message or a traceback.
- [ ] The trust model is documented for embedders.
**Verification:**
- [ ] Error fixtures; suite; sanitizers.
**Dependencies:** T86-T98.
**Files:** `src/net.c`, `docs/`, `tests/errors`.
**Scope:** M.

### T100 - Documentation
**Description:** A "Networking" section in the language reference (natives, objects,
methods, examples, limits), an architecture section (descriptor/GC model, scheduler
integration, poll loop), README features and native tables, and the limits section.
**Acceptance criteria:**
- [ ] Every native and method is documented with its signature and error behavior, and the examples run as written.
- [ ] `AGENTS.md` records any dependency decision; `CHANGELOG.md` gets the new entries.
**Verification:**
- [ ] Examples executed; native list cross-checked against the docs table; suite.
**Dependencies:** T82-T99.
**Files:** `docs/language.md`, `docs/architecture.md`, `README.md`, `CHANGELOG.md`, `AGENTS.md`.
**Scope:** M.

### T101 - Benchmarks for the network layer
**Description:** Benchmarks for local HTTP round-trips and echo throughput, wired into
`benchmarks/run_all.py`, plus the documented CI-safe testing strategy (local servers,
ephemeral ports, no public network).
**Acceptance criteria:**
- [ ] Both benchmarks report min/median and run offline in CI within the existing budget.
**Verification:**
- [ ] `python3 benchmarks/run_all.py`; CI green.
**Dependencies:** T92, T90.
**Files:** `benchmarks/`, `docs/architecture.md`.
**Scope:** S.

### T102 - Example programs
**Description:** `examples/http_client.spark`, `examples/echo_server.spark` and the
Discord bot from T98, plus README pointers.
**Acceptance criteria:**
- [ ] Each example runs against a local counterpart and is listed in the README.
**Verification:**
- [ ] Manual run of each example; suite (the examples stay lint-clean).
**Dependencies:** T93, T98.
**Files:** `examples/`, `README.md`.
**Scope:** S.

## Checkpoint: Complete
- [ ] All acceptance criteria met; suite, sanitizers and benchmark gates green
- [ ] Ready for review
