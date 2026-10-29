# Implementation Plan: Spark networking work stream

The 0.1.0 roadmap (phases 1-14, tasks T1-T81, decisions A1-A8) is complete and its
sections were removed to keep this file short. Its record survives in git history, in
`CHANGELOG.md` and in `docs/intent/spark-completion.md`. Phase, task and decision
numbering continue from it (phases 15-20, T82+, A9+), and everything below is the
active work.

## Definition of Done (every task)

- Build is warning-free: `make` with `-O3 -Wall -Wextra -Wpedantic`.
- `python3 tests/run_tests.py` passes (new behavior has new fixtures).
- Sanitizer run is clean when the task touches the VM/GC/compiler:
  `make clean && make CFLAGS="-std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic" LDFLAGS="-fsanitize=address,undefined" && python3 tests/run_tests.py && make clean && make`.
- `python3 benchmarks/run.py` minimum stays **<= 5 ms** unless the task is explicitly
  about performance (then state the target).
- Documentation updated: `README.md`, `docs/language.md`, `docs/architecture.md`.
- All artifacts in English (`AGENTS.md`).

## Baseline (for regression checks)

- Tests: 208 passing (`python3 tests/run_tests.py`), also clean under ASan/UBSan.
- Benchmark: `fib(27)` minimum 4.94 ms, median 5.01 ms on an idle machine
  (`python3 benchmarks/run.py`; a few percent of variance under load).
- No warnings with `-O3 -Wall -Wextra -Wpedantic`.

## Overview

Spark has no networking at all. The native surface today is text, collections, math,
files, processes, JSON, regex, time and randomness (see `docs/language.md` section 10),
and the only way to reach the network is `exec("curl ...")`, which pays for a process
per request, cannot stream, and pushes untrusted text through a shell.

This stream adds a real network stack: TCP and UDP sockets as GC-managed resources,
asynchronous waits integrated with the existing coroutine scheduler, an HTTP/1.1
client, TLS, and WebSocket - enough to write a Discord gateway client in Spark without
leaving the language.

The language side already has everything a network library needs (maps, JSON, classes,
exceptions, closures, coroutines, regex and string tools). The missing piece is
entirely the native I/O boundary, so the work is concentrated in `src/net.c` plus a
small amount of VM plumbing.

## Architecture Decisions

- **A9 - Sockets are GC-managed resources**, modeled on `ObjFile`: `ObjSocket` and
  `ObjServer` own a file descriptor and are closed by `close()`, by the collector's
  finalizer, and on VM shutdown. Using a closed handle is a runtime error, and
  `type()` reports `"socket"` / `"server"`.
- **A10 - A network operation that would block yields the coroutine** (decided, Q12),
  exactly like a channel wait (new `CORO_WAIT_*` kinds plus a poll step in
  `vm_scheduler`), so one process serves many connections with no threads. The existing
  guard that rejects a blocking operation inside a native callback or an imported
  module (`run_depth > 1`) applies to these waits too. If the T87 spike shows the
  integration is unworkable, the documented fallback is blocking calls with timeouts.
- **A11 - Network failures get their own error categories** (`ERROR_NETWORK`,
  `ERROR_TIMEOUT`) so `catch (e) { e.type }` distinguishes a refused connection from a
  timeout and from a bad host.
- **A12 - Every operation is bounded**: connect/accept/read/write timeouts, maximum
  message and body sizes, redirect caps and connection limits, each with an explicit
  error instead of an unbounded wait or allocation.
- **A13 - Secure by default**: TLS verifies the certificate chain and the hostname and
  cannot be silently disabled; the trust model, the SSRF surface and secret handling
  are documented.
- **A14 - The socket, HTTP and WebSocket layers add no dependencies.** TLS is the one
  approved exception (decided, Q11): OpenSSL, used only by the TLS natives, documented
  in `AGENTS.md` and `CHANGELOG.md` when T94 lands.
- **A15 - Tests never touch the public internet**: local servers only (Python stdlib
  for the harness, Spark's own server natives where possible), ephemeral ports and
  generous timeouts, so the suite stays deterministic in CI.

## Task List

### Phase 15 - Sockets (blocking core, no new dependencies)
- [x] T82 `ObjSocket`/`ObjServer`: the resource object model and `src/net.c` skeleton
- [x] T83 TCP client: `tcp_connect`, `send`, `recv`, `recv_line`, timeouts
- [x] T84 TCP server: `tcp_listen`, `accept`, `close`, ephemeral ports
- [x] T85 Name resolution, IPv4/IPv6, error categories and fd limits
- [x] T86 UDP: `udp_bind`, `send_to`, `recv_from`

### Checkpoint: Phase 15
- [x] Suite green (optimized and ASan/UBSan), `-Werror` build
- [x] A Spark client talks to a Spark server on localhost
- [ ] Benchmark: `fib(27)` delta is heap-layout noise (see `tasks/todo.md`)

### Phase 16 - Asynchronous I/O on the coroutine scheduler (highest risk)
- [x] T87 Spike + `sock.wait(read?/write?, timeout)`: a blocked socket wait yields the coroutine
- [x] T88 `net_select(readables, timeout)` and timer-aware polling
- [x] T89 Coroutine-aware `accept`/`recv`/`send` with the `run_depth` guard
- [x] T90 One-process echo server under load (N clients, idle CPU) and fixtures

### Checkpoint: Phase 16
- [x] N concurrent connections served by one process with no busy-wait
- [x] Benchmark minimum below 5 ms (measured 4.97 ms); sanitizers clean; `poll(2)` chosen

### Phase 17 - HTTP/1.1 client and server
- [x] T91 `url_parse`, `url_encode`, `url_decode`, `parse_query`
- [x] T92 HTTP/1.1 client: status, headers, chunked, redirects, caps, timeouts
- [x] T93 HTTP server as a Spark library on the socket layer, with fixtures

### Checkpoint: Phase 17
- [x] A Spark program fetches a local HTTP endpoint and a Spark server answers a real client
- [x] Header/body/redirect/timeout limits enforced and covered by error fixtures

### Phase 18 - TLS (decision-gated on Q11)
- [x] T94 `tls_connect` + `read`/`write`/`close` with certificate and hostname verification
- [x] T95 HTTPS through the HTTP client; verification failure fixtures

### Checkpoint: Phase 18
- [x] Self-signed and mismatched-host certificates are rejected and covered by tests
- [x] Dependency decision recorded in `AGENTS.md` and `CHANGELOG.md`

### Phase 19 - WebSocket (enables the Discord gateway)
- [ ] T96 WebSocket client: upgrade handshake, masking, text/binary/ping/close, fragmentation
- [ ] T97 (optional) WebSocket server side
- [ ] T98 End-to-end against a local mock gateway + a documented Discord bot example

### Checkpoint: Phase 19
- [ ] The mock-gateway fixture passes offline; the example is documented step by step

### Phase 20 - Hardening, docs and ecosystem
- [ ] T99 Security hardening pass (limits, SSRF, secrets, sandboxing caveat)
- [ ] T100 Documentation: language reference, architecture, README, limits, CHANGELOG
- [ ] T101 Benchmarks: HTTP round-trip and echo throughput, plus the CI-safe test strategy
- [ ] T102 Example programs: HTTP client CLI, echo server, Discord bot

### Checkpoint: Complete
- [ ] All acceptance criteria met; suite, sanitizers and benchmark gates green
- [ ] Ready for review

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| The async integration destabilizes the scheduler or the benchmark | High | Phase 16 opens with a spike; blocking core lands first; benchmark and sanitizer gates; documented fallback to blocking-with-timeouts |
| TLS forces an external dependency, contradicting `AGENTS.md` | High | Decision gate Q11 before Phase 18; the fallback keeps sockets and WebSocket over `ws://` with an external tunnel |
| A verification bug silently accepts a wrong certificate | High | Verify-by-default with rejection fixtures; dedicated security review of T94 (see the security checklist) |
| Flaky or hanging tests (ports, timeouts, CI network) | Medium | Ephemeral ports, local servers only, generous-but-bounded timeouts, no public internet |
| File-descriptor leaks | Medium | Finalizer plus `close()`, ASan runs and an fd-count assertion in the fixtures |
| Platform drift (Windows, macOS) | Medium | `_POSIX_C_SOURCE` sockets; WSL documented as the supported Windows path; CI on Linux |
| A blocking call stalls every coroutine before Phase 16 | Medium | Timeouts on every operation; the limitation is documented until the async phase lands |
| Memory growth from large bodies or frames | Medium | Size caps, streaming `read`, and explicit errors instead of unbounded buffers |

## Parallelization

- Independent: documentation (T100) and examples (T102) against the agreed API; the
  Python test servers and the TLS certificate fixture; UDP (T86) alongside the TCP
  client (T83).
- Sequential: T82 before anything else; T87-T89 are one chain; T94 before T95; T96
  before T98.
- Coordinate first: the socket API surface (names, return shapes, error categories)
  must be frozen after T85, because T92, T94 and T96 build on it.

## Open Questions

Decided on 2026-10-28 (recorded in `docs/intent/networking.md`):

- **Q11 (TLS and dependencies) - DECIDED:** link OpenSSL for the TLS natives only, as
  the single documented exception to "no external dependencies". `https://` and
  `wss://` are in scope, which is what makes a Discord gateway client reachable.
- **Q12 (blocking semantics) - DECIDED:** an operation that would block yields the
  coroutine, consistent with channels. The `run_depth > 1` guard extends to these
  waits.
- **Q13 (HTTP server scope) - DECIDED (phase 17):** an HTTP server is in scope, built
  as a Spark library on the socket layer (`lib/http.spark`) so the native surface
  stays small, rather than as a large set of server natives.
- **Q14 (test certificates) - DECIDED (phase 18):** commit a throwaway CA and leaf
  certificates under `tests/tls/`, with a regeneration script. The private keys are
  public fixtures, valid only for localhost and never trusted outside the suite, so
  the tests stay deterministic without requiring `openssl` at run time.
- **Q15 (UDP) - DECIDED (phase 15):** UDP is in scope and implemented as T86
  (`udp_bind`, `send_to`, `recv_from`).
- **Q16 (sandboxing) - DECIDED (phase 18):** document plainly that Spark is not a
  sandbox and that `exec` already grants network access; this stream adds no
  restricted mode. The network-security note in `docs/language.md` records it.

## Out of Scope

- HTTP/2, HTTP/3/QUIC, proxies, and a Discord client with sharding, rate-limit
  bookkeeping or voice.
- DNS-over-TLS, mDNS/DNS-SD and any non-blocking DNS resolver.
- A thread-based concurrency model: coroutines stay cooperative and single-threaded.
- Windows-native sockets; WSL stays the supported Windows path.
