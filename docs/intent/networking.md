# Intent: networking

Confirmed with the user on 2026-10-28. This records the agreed scope, decisions and
work order for the networking work stream. It is the input for phases 15-20 in
`tasks/plan.md` and `tasks/todo.md`; update it if the intent changes.

## Outcome

Spark can speak to the network from the language itself: TCP and UDP sockets as
GC-managed resources, waits that suspend only their coroutine, an HTTP/1.1 client, TLS
with certificate verification, and a WebSocket client - enough to write a Discord
gateway client in Spark without shelling out to `curl`.

## Why now

Today the only way to reach the network is `exec("curl ...")`: one process per
request, no streaming, and untrusted text crossing a shell. The language side (maps,
JSON, classes, exceptions, closures, coroutines, regex) already has what a networking
library needs, so the whole gap is the native I/O boundary.

## Success criteria

- Warning-free build, full suite green, sanitizers clean.
- `fib(27)` minimum still below 5 ms after every phase.
- Tests never touch the public internet: local servers, ephemeral ports, bounded
  timeouts.
- TLS verifies the chain and the hostname by default, with rejection fixtures.
- `docs/language.md`, `docs/architecture.md`, `README.md`, the limits section,
  `AGENTS.md` and `CHANGELOG.md` match the code.

## Decisions

- **TLS is the one approved dependency.** Link OpenSSL, used only by the TLS natives
  (T94/T95), documented as the single exception to "no external dependencies" in
  `AGENTS.md` and `CHANGELOG.md`. The socket, HTTP and WebSocket layers stay
  dependency-free.
- **A blocking network operation yields the coroutine** rather than stalling the
  process, consistent with channel waits. The existing `run_depth > 1` guard (no
  blocking inside a native callback or an imported module) extends to these waits.
- **Sockets are GC-managed resources**, modeled on `ObjFile`: own descriptor,
  `close()`, collector finalizer, closed on VM shutdown.
- **Errors are categorized** (`ERROR_NETWORK`, `ERROR_TIMEOUT`) so `catch` can tell a
  refusal from a timeout.
- **Everything is bounded**: timeouts, connection/descriptor caps, body/header/frame
  size limits and redirect limits, each with an explicit error.
- **Secure by default**: certificate and hostname verification cannot be silently
  disabled; the trust model and the SSRF surface are documented, including the plain
  statement that `exec` already grants network access (Spark is not a sandbox).
- **Test certificates are committed fixtures.** A throwaway CA and leaf certificates
  live under `tests/tls/`, with a regeneration script. Their private keys are public,
  valid only for localhost and never trusted outside the suite, so TLS tests stay
  deterministic without requiring `openssl` at run time.

## Work order

1. **Sockets (T82-T86),** no new dependencies: resource objects, TCP client/server,
   resolution and error taxonomy, UDP. Checkpoint before touching the scheduler.
2. **Asynchronous waits (T87-T90),** the highest-risk change: start with a spike that
   makes one wait yield, then generalize to multiple descriptors and timers, route the
   blocking operations through it, and prove it under concurrent load without
   regressing the benchmark.
3. **HTTP/1.1 (T91-T93):** URL tools, a client with chunked bodies, redirects and
   limits, and a server built as a Spark library rather than a giant native.
4. **TLS (T94-T95):** verification by default, then HTTPS through the HTTP client.
5. **WebSocket (T96-T98):** framing and handshake, then a local mock gateway and the
   Discord bot example that runs against it.
6. **Hardening and documentation (T99-T102):** security pass, docs, benchmarks and
   example programs.

## Out of scope

- HTTP/2, HTTP/3/QUIC, proxies, and a full Discord client (sharding, rate-limit
  bookkeeping, voice).
- Thread-based concurrency: coroutines stay cooperative and single-threaded.
- Windows-native sockets; WSL stays the supported Windows path.
- A non-blocking DNS resolver (resolution uses `getaddrinfo`).
