#!/usr/bin/env python3
"""Automated test suite for the Spark language."""

from __future__ import annotations

import base64
import difflib
import hashlib
import json
import os
import select
import socket
import socketserver
import ssl
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "spark"
CASES = ROOT / "tests" / "cases"
ERRORS = ROOT / "tests" / "errors"
TLS = ROOT / "tests" / "tls"

results: list[tuple[str, bool, str]] = []


def report(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(("  ok     " if ok else "  FAIL   ") + name)
    if not ok and detail:
        for line in detail.rstrip().splitlines():
            print("         " + line)


def run(args: list[str], stdin: str | None = None, timeout: int = 60, env: dict[str, str] | None = None):
    environment = None if env is None else {**os.environ, **env}
    return subprocess.run(
        [str(BINARY), *args],
        input=stdin,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
        env=environment,
    )


def unified(expected: str, actual: str) -> str:
    return "".join(
        difflib.unified_diff(
            expected.splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile="expected",
            tofile="actual",
        )
    )


def parse_spec(path: Path) -> tuple[int, str]:
    exit_code = 0
    stderr_fragment = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("exit:"):
            exit_code = int(line.split(":", 1)[1].strip())
        elif line.startswith("stderr:"):
            stderr_fragment = line.split(":", 1)[1].strip()
    return exit_code, stderr_fragment


def test_cases() -> None:
    print("Valid programs")
    for program in sorted(CASES.glob("*.spark")):
        expected_path = program.with_suffix(".out")
        if not expected_path.exists():
            report(program.name, False, "missing the .out file with the expected output")
            continue

        expected = expected_path.read_text(encoding="utf-8")
        try:
            completed = run([str(program)])
        except subprocess.TimeoutExpired:
            report(program.name, False, "the program did not finish within 60 seconds")
            continue

        detail = ""
        ok = completed.returncode == 0 and completed.stdout == expected and not completed.stderr
        if completed.returncode != 0:
            detail += f"exit code {completed.returncode} (expected 0)\n"
        if completed.stdout != expected:
            detail += unified(expected, completed.stdout)
        if completed.stderr:
            detail += "unexpected stderr:\n" + completed.stderr
        report(program.name, ok, detail)


def test_errors() -> None:
    print("\nExpected errors")
    for program in sorted(ERRORS.glob("*.spark")):
        spec_path = program.with_suffix(".out")
        if not spec_path.exists():
            report(program.name, False, "missing the .out file with the specification")
            continue

        expected_exit, expected_stderr = parse_spec(spec_path)
        try:
            completed = run([str(program)])
        except subprocess.TimeoutExpired:
            report(program.name, False, "the program did not finish within 60 seconds")
            continue

        detail = ""
        ok = True
        if completed.returncode != expected_exit:
            ok = False
            detail += f"exit code {completed.returncode}, expected {expected_exit}\n"
        if expected_stderr and expected_stderr not in completed.stderr:
            ok = False
            detail += f"stderr does not contain \u00ab{expected_stderr}\u00bb:\n{completed.stderr}"
        report(program.name, ok, detail)


def test_repl() -> None:
    print("\nREPL")
    session = (
        "1 + 2;\n"
        "_ * 3;\n"
        "var x = 5;\n"
        "x * 2;\n"
        "fn doble(n) {\n"
        "  return n * 2;\n"
        "}\n"
        "doble(21);\n"
        'print("done");\n'
        "exit\n"
    )
    completed = run([], stdin=session)
    checks = {
        "prints the result of an expression": "spark> 3" in completed.stdout,
        "captures last expression in _": "spark> 9" in completed.stdout,
        "keeps global variables": "spark> 10" in completed.stdout,
        "accepts multi-line blocks": "spark> 42" in completed.stdout,
        "runs print": "done" in completed.stdout,
        "exits with code 0": completed.returncode == 0,
    }
    for name, ok in checks.items():
        report(name, ok, completed.stdout + completed.stderr if not ok else "")

    failure = run([], stdin='1 / 0;\nprint("still");\nexit\n')
    report(
        "a runtime error does not close the REPL",
        failure.returncode == 0 and "still" in failure.stdout,
        failure.stdout + failure.stderr,
    )

    constants = run([], stdin="const x = 1;\nx = 2;\nprint(x);\nexit\n")
    report(
        "a const stays immutable across REPL inputs",
        constants.returncode == 0
        and "cannot assign to constant 'x'" in constants.stderr
        and "spark> 1" in constants.stdout,
        constants.stdout + constants.stderr,
    )

    failed_const = run([], stdin="const x = 1 / 0;\nconst x = 2;\nprint(x);\nexit\n")
    report(
        "a failed const declaration does not reserve the name",
        failed_const.returncode == 0
        and "division by zero" in failed_const.stderr
        and "cannot redefine constant 'x'" not in failed_const.stderr
        and "spark> 2" in failed_const.stdout,
        failed_const.stdout + failed_const.stderr,
    )


def test_error_snippets() -> None:
    print("\nError snippets")
    runtime = run([str(ERRORS / "tipos.spark")])
    report(
        "a runtime error shows the failing line and a caret",
        runtime.returncode == 70
        and 'print(1 + "a");' in runtime.stderr
        and "^" in runtime.stderr,
        runtime.stderr,
    )

    syntax = run([str(ERRORS / "falta_punto_y_coma.spark")])
    report(
        "a syntax error shows the failing line and a caret",
        syntax.returncode == 65
        and "print(x);" in syntax.stderr
        and "^" in syntax.stderr,
        syntax.stderr,
    )


def test_cli() -> None:
    print("\nCommand-line interface")
    version = run(["--version"])
    report("--version", version.returncode == 0 and "Spark" in version.stdout,
           version.stdout + version.stderr)

    help_run = run(["--help"])
    report("--help", help_run.returncode == 0 and "Usage: spark" in help_run.stdout,
           help_run.stdout + help_run.stderr)

    dump = run(["--dump", str(CASES / "hola.spark")])
    ok = (
        dump.returncode == 0
        and "CONSTANT" in dump.stdout
        and "GET_GLOBAL" in dump.stdout
        and "RETURN" in dump.stdout
    )
    report("--dump shows the bytecode", ok, dump.stdout + dump.stderr)

    missing = run([str(ROOT / "tests" / "missing.spark")])
    report("missing file returns 74", missing.returncode == 74,
           missing.stdout + missing.stderr)

    exit_script = ROOT / "tests" / "test_exit.spark"
    exit_script.write_text("exit(42);\n", encoding="utf-8")
    try:
        exit_run = run([str(exit_script)])
        report("exit(code) sets returncode", exit_run.returncode == 42,
               f"expected 42, got {exit_run.returncode}")
    finally:
        if exit_script.exists():
            exit_script.unlink()

    argv_script = ROOT / "tests" / "test_argv.spark"
    argv_script.write_text("print(len(argv), argv[1], argv[2]);\n", encoding="utf-8")
    try:
        argv_run = run([str(argv_script), "hello", "world"])
        report("argv captures CLI arguments",
               argv_run.returncode == 0 and "3 hello world" in argv_run.stdout,
               argv_run.stdout + argv_run.stderr)
    finally:
        if argv_script.exists():
            argv_script.unlink()

    # Precompiled bytecode tests (.sparkc)
    hello_src = CASES / "hola.spark"
    default_sparkc = CASES / "hola.sparkc"
    custom_sparkc = ROOT / "tests" / "custom.sparkc"
    try:
        c1 = run(["--compile", str(hello_src)])
        report("--compile generates .sparkc file",
               c1.returncode == 0 and default_sparkc.exists(),
               c1.stdout + c1.stderr)

        r1 = run([str(default_sparkc)])
        report("running .sparkc file produces expected output",
               r1.returncode == 0 and "Hello, world!" in r1.stdout,
               r1.stdout + r1.stderr)

        c2 = run(["--compile", str(hello_src), "-o", str(custom_sparkc)])
        report("--compile -o generates custom bytecode file",
               c2.returncode == 0 and custom_sparkc.exists(),
               c2.stdout + c2.stderr)

        r2 = run([str(custom_sparkc)])
        report("running custom .sparkc file produces expected output",
               r2.returncode == 0 and "Hello, world!" in r2.stdout,
               r2.stdout + r2.stderr)
    finally:
        if default_sparkc.exists():
            default_sparkc.unlink()
        if custom_sparkc.exists():
            custom_sparkc.unlink()

    bad_magic = ROOT / "tests" / "bad_magic.sparkc"
    bad_magic.write_bytes(b"BADM\x01\x00\x00\x00")
    try:
        bm = run([str(bad_magic)])
        report(".sparkc with invalid magic exits with code 65",
               bm.returncode == 65 and "incorrect magic header" in bm.stderr,
               bm.stdout + bm.stderr)
    finally:
        if bad_magic.exists():
            bad_magic.unlink()

    bad_version = ROOT / "tests" / "bad_version.sparkc"
    bad_version.write_bytes(b"SPRK\x99\x00\x00\x00")
    try:
        bv = run([str(bad_version)])
        report(".sparkc with unsupported version exits with code 65",
               bv.returncode == 65 and "unsupported bytecode version" in bv.stderr,
               bv.stdout + bv.stderr)
    finally:
        if bad_version.exists():
            bad_version.unlink()

    # Debugger tests (--debug)
    dbg_script = ROOT / "tests" / "test_debug.spark"
    dbg_script.write_text(
        "fn add(x, y) {\n"
        "    var z = x + y;\n"
        "    return z;\n"
        "}\n"
        "var a = 10;\n"
        "var b = 20;\n"
        "var r = add(a, b);\n"
        "print(\"ans:\", r);\n",
        encoding="utf-8",
    )
    try:
        dbg_run = run(["--debug", str(dbg_script)], stdin="b 3\nc\nl\nbt\ns\nl\nc\n")
        report("--debug supports breakpoints, continue, locals, and step",
               dbg_run.returncode == 0
               and "Breakpoint 1 hit" in dbg_run.stdout
               and "x = 10" in dbg_run.stdout
               and "y = 20" in dbg_run.stdout
               and "ans: 30" in dbg_run.stdout,
               dbg_run.stdout + dbg_run.stderr)

        # Debugger scoped locals test across slot reuse
        dbg_scope = ROOT / "tests" / "test_dbg_scope.spark"
        dbg_scope.write_text(
            "{\n"
            "    var a = 100;\n"
            "}\n"
            "{\n"
            "    var b = 200;\n"
            "    print(b);\n"
            "}\n",
            encoding="utf-8"
        )
        dbg_scope_run = run(["--debug", str(dbg_scope)], stdin="b 6\nc\nl\nc\n")
        report("--debug respects lexical scope for locals across block slot reuse",
               dbg_scope_run.returncode == 0
               and "  b = 200" in dbg_scope_run.stdout
               and "  a = 100" not in dbg_scope_run.stdout,
               dbg_scope_run.stdout + dbg_scope_run.stderr)
        if dbg_scope.exists():
            dbg_scope.unlink()
    finally:
        if dbg_script.exists():
            dbg_script.unlink()

    # Formatter tests (--format)
    fmt_script = ROOT / "tests" / "test_fmt.spark"
    fmt_script.write_text("var  x=1+2;   if(x>2){print(x);}\n", encoding="utf-8")
    try:
        f1 = run(["--format", str(fmt_script)])
        expected_fmt = "var x = 1 + 2;\nif (x > 2) {\n    print(x);\n}\n"
        report("--format canonicalizes code to stdout",
               f1.returncode == 0 and f1.stdout == expected_fmt,
               f1.stdout)

        f_write = run(["--format", "-w", str(fmt_script)])
        report("--format -w writes in-place and is idempotent",
               f_write.returncode == 0 and fmt_script.read_text(encoding="utf-8") == expected_fmt,
               fmt_script.read_text(encoding="utf-8"))

        fmt_advanced = ROOT / "tests" / "test_fmt_adv.spark"
        fmt_advanced.write_text(
            "var x = 1; // comment\n"
            "var y = cond ? 1 : 2;\n"
            "var m = {\"a\": 1};\n"
            "print({\"k\": 2});\n",
            encoding="utf-8"
        )
        f_adv = run(["--format", str(fmt_advanced)])
        report("--format preserves trailing comments and formats ternaries and maps",
               f_adv.returncode == 0 and
               "var x = 1; // comment\n" in f_adv.stdout and
               "var y = cond ? 1 : 2;\n" in f_adv.stdout and
               "var m = {\"a\": 1};\n" in f_adv.stdout and
               "print({\"k\": 2});\n" in f_adv.stdout,
               f_adv.stdout)
        if fmt_advanced.exists():
            fmt_advanced.unlink()

        fmt_modern = ROOT / "tests" / "test_fmt_modern.spark"
        fmt_modern.write_text(
            'var s = """line one\n'
            'line two""";\n'
            'var a=[1,2,3];\n'
            'var x=a[1:2];\n'
            'var y=2**3;\n'
            'x++;\n'
            'var z=--y;\n'
            'var o=nil;\n'
            'var q=o?.value;\n'
            'var w=fn(n)=>n+1;\n'
            'var r=1 in a;\n'
            'var t=nil;\n'
            't??=5;\n'
            'print(s,x,y,z,q,w(1),r,t);\n',
            encoding="utf-8",
        )
        try:
            f_modern = run(["--format", str(fmt_modern)])
            expected_modern = (
                'var s = """line one\n'
                'line two""";\n'
                'var a = [1, 2, 3];\n'
                'var x = a[1:2];\n'
                'var y = 2 ** 3;\n'
                'x++;\n'
                'var z = --y;\n'
                'var o = nil;\n'
                'var q = o?.value;\n'
                'var w = fn(n) => n + 1;\n'
                'var r = 1 in a;\n'
                'var t = nil;\n'
                't ??= 5;\n'
                'print(s, x, y, z, q, w(1), r, t);\n'
            )
            report("--format handles modern syntax",
                   f_modern.returncode == 0 and f_modern.stdout == expected_modern,
                   f_modern.stdout)
        finally:
            if fmt_modern.exists():
                fmt_modern.unlink()
    finally:
        if fmt_script.exists():
            fmt_script.unlink()

    # Linter tests (--lint)
    lint_clean = ROOT / "tests" / "test_lint_clean.spark"
    lint_clean.write_text("var a = 10;\nprint(a);\n", encoding="utf-8")
    lint_modern = ROOT / "tests" / "test_lint_modern.spark"
    lint_modern.write_text(
        "var arr = [1, 2, 3];\n"
        "var [first, second] = arr;\n"
        "var s = 0;\n"
        "for (var i, value in arr) { s += i + value; }\n"
        "var f = fn(x) => x + first;\n"
        "var tail = arr[1:];\n"
        "var maybe = nil;\n"
        "var guarded = maybe?.value;\n"
        "var ok = first in arr and first == second;\n"
        "print(second, s, f(1), tail, guarded, ok);\n",
        encoding="utf-8",
    )
    lint_dirty = ROOT / "tests" / "test_lint_dirty.spark"
    lint_dirty.write_text(
        "fn test(unused_param, used_param) {\n"
        "    var x = used_param;\n"
        "    if (true) {\n"
        "        var x = 99;\n"
        "        print(x);\n"
        "    }\n"
        "    return x;\n"
        "    print(\"unreachable\");\n"
        "}\n"
        "test(1, 2);\n",
        encoding="utf-8",
    )
    try:
        l_clean = run(["--lint", str(lint_clean)])
        report("--lint returns 0 on clean code",
               l_clean.returncode == 0 and l_clean.stdout == "",
               l_clean.stdout + l_clean.stderr)

        l_dirty = run(["--lint", str(lint_dirty)])
        report("--lint detects unused variables, unreachable code, and shadowing",
               l_dirty.returncode == 1
               and "[unused-variable]" in l_dirty.stdout
               and "[unreachable-code]" in l_dirty.stdout
               and "[shadowed-variable]" in l_dirty.stdout,
               l_dirty.stdout + l_dirty.stderr)

        l_modern = run(["--lint", str(lint_modern)])
        report("--lint understands modern syntax",
               l_modern.returncode == 0 and l_modern.stdout == "",
               l_modern.stdout + l_modern.stderr)
    finally:
        if lint_clean.exists():
            lint_clean.unlink()
        if lint_dirty.exists():
            lint_dirty.unlink()
        if lint_modern.exists():
            lint_modern.unlink()


def test_network_limits() -> None:
    print("\nNetwork limits")
    script = ROOT / "tests" / "net_fd_exhausted.spark"
    # A low RLIMIT_NOFILE makes descriptor exhaustion deterministic. bash drops
    # the limit before exec, so it applies to the interpreter itself.
    completed = subprocess.run(
        ["bash", "-c", f"ulimit -n 64; exec '{BINARY}' '{script}'"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=60,
    )
    report(
        "descriptor exhaustion raises a categorized error",
        completed.returncode == 0
        and "NetworkError: too many open descriptors" in completed.stdout,
        completed.stdout + completed.stderr,
    )


class ScriptedHandler(BaseHTTPRequestHandler):
    """A scripted local HTTP server: every path gets a fixed response, so the
    client fixtures never touch the public internet."""

    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", headers=()):
        self.send_response(code)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        path = self.path
        if path == "/hello":
            self.reply(200, b"hello world\n", [("X-Test", "yes")])
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for chunk in (b"chunk one", b"two"):
                self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/redirect":
            self.reply(302, b"", [("Location", "/hello")])
        elif path == "/loop":
            self.reply(302, b"", [("Location", "/loop")])
        elif path == "/big":
            self.reply(200, b"x" * 100000)
        elif path == "/slow":
            time.sleep(1)
            self.reply(200, b"late")
        elif path == "/huge":
            # A header line well past the 64 KiB response cap.
            self.wfile.write(b"HTTP/1.1 200 OK\r\nX-Huge: " + b"a" * 70000
                             + b"\r\nContent-Length: 0\r\n\r\n")
        elif path == "/headers":
            # One header past the 1000-header response cap.
            self.wfile.write(b"HTTP/1.1 200 OK\r\n" + b"X-Filler: a\r\n" * 1001
                             + b"Content-Length: 0\r\n\r\n")
        elif path == "/big-chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for _ in range(2):
                chunk = b"x" * 800
                self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/bad":
            self.wfile.write(b"not an http response\r\n\r\n")
        elif path == "/eof":
            self.close_connection = True
        elif path == "/hdr":
            self.reply(200, self.headers.get("X-Probe", "").encode("utf-8"))
        else:
            self.reply(404, b"not found")

    def do_POST(self):
        if self.path == "/echo":
            length = int(self.headers.get("Content-Length", "0"))
            self.reply(200, self.rfile.read(length))
        else:
            self.reply(404, b"not found")


class QuietHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass


class TlsEchoHandler(socketserver.BaseRequestHandler):
    """Echoes one TLS payload and closes, so the fixtures can check read/EOF."""

    def handle(self):
        try:
            data = self.request.recv(4096)
            if data:
                self.request.sendall(b"echo:" + data)
        except OSError:
            pass


class StallHandler(socketserver.BaseRequestHandler):
    """Accepts a connection but never answers the TLS handshake."""

    def handle(self):
        time.sleep(2.0)


class QuietTCPServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass


def tls_context(cert_name: str) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(TLS / f"{cert_name}.pem", TLS / f"{cert_name}.key")
    return context


def start_tls_echo(cert_name: str) -> QuietTCPServer:
    server = QuietTCPServer(("127.0.0.1", 0), TlsEchoHandler)
    server.socket = tls_context(cert_name).wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def start_stall() -> QuietTCPServer:
    server = QuietTCPServer(("127.0.0.1", 0), StallHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def start_tls_http(cert_name: str) -> QuietHTTPServer:
    server = QuietHTTPServer(("127.0.0.1", 0), ScriptedHandler)
    server.socket = tls_context(cert_name).wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def run_fixture(script: str, expected_name: str, args: list[str], label: str,
                timeout: int = 60) -> None:
    completed = run([str(ROOT / "tests" / script), *args], timeout=timeout)
    expected = (ROOT / "tests" / expected_name).read_text(encoding="utf-8")
    detail = ""
    ok = completed.returncode == 0 and completed.stdout == expected and not completed.stderr
    if completed.returncode != 0:
        detail += f"exit code {completed.returncode} (expected 0)\n"
    if completed.stdout != expected:
        detail += unified(expected, completed.stdout)
    if completed.stderr:
        detail += "unexpected stderr:\n" + completed.stderr
    report(label, ok, detail)


def test_tls_client() -> None:
    print("\nTLS client")
    servers: list[QuietTCPServer] = []
    try:
        trusted = start_tls_echo("server")
        servers.append(trusted)
        wrong = start_tls_echo("wrong-host")
        servers.append(wrong)
        self_signed = start_tls_echo("self-signed")
        servers.append(self_signed)
        untrusted = start_tls_echo("untrusted")
        servers.append(untrusted)
        stalled = start_stall()
        servers.append(stalled)
        run_fixture(
            "tls_client.spark",
            "tls_client.out",
            [
                str(trusted.server_address[1]),
                str(wrong.server_address[1]),
                str(self_signed.server_address[1]),
                str(untrusted.server_address[1]),
                str(TLS / "ca.pem"),
                str(stalled.server_address[1]),
            ],
            "tls transport verifies chains and host names",
        )
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


def test_https_client() -> None:
    print("\nHTTPS client")
    servers: list[QuietHTTPServer] = []
    try:
        trusted = start_tls_http("server")
        servers.append(trusted)
        wrong = start_tls_http("wrong-host")
        servers.append(wrong)
        self_signed = start_tls_http("self-signed")
        servers.append(self_signed)
        untrusted = start_tls_http("untrusted")
        servers.append(untrusted)
        run_fixture(
            "https_client.spark",
            "https_client.out",
            [
                str(trusted.server_address[1]),
                str(wrong.server_address[1]),
                str(self_signed.server_address[1]),
                str(untrusted.server_address[1]),
                str(TLS / "ca.pem"),
            ],
            "https client injects a CA and rejects bad certificates",
        )
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_frame(opcode: int, payload: bytes = b"", fin: bool = True) -> bytes:
    first = (0x80 if fin else 0) | opcode
    length = len(payload)
    if length <= 125:
        return bytes([first, length]) + payload
    if length <= 0xFFFF:
        return bytes([first, 126]) + struct.pack(">H", length) + payload
    return bytes([first, 127]) + struct.pack(">Q", length) + payload


def ws_masked_frame(opcode: int, payload: bytes) -> bytes:
    mask = b"\x01\x02\x03\x04"
    body = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
    return bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + body


def ws_recv_exact(sock, count: int, buffered: bytes) -> tuple[bytes, bytes]:
    while len(buffered) < count:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("connection closed")
        buffered += chunk
    return buffered[:count], buffered[count:]


def ws_read_frame(sock, buffered: bytes) -> tuple[int, bytes, bytes]:
    header, buffered = ws_recv_exact(sock, 2, buffered)
    first, second = header[0], header[1]
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        extra, buffered = ws_recv_exact(sock, 2, buffered)
        length = struct.unpack(">H", extra)[0]
    elif length == 127:
        extra, buffered = ws_recv_exact(sock, 8, buffered)
        length = struct.unpack(">Q", extra)[0]
    mask = b""
    if masked:
        mask, buffered = ws_recv_exact(sock, 4, buffered)
    payload, buffered = ws_recv_exact(sock, length, buffered)
    if masked:
        payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
    return opcode, payload, buffered


def ws_read_until_close(sock, buffered: bytes) -> None:
    while True:
        opcode, payload, buffered = ws_read_frame(sock, buffered)
        if opcode == 8:
            sock.sendall(ws_frame(8, payload[:2]))
            return
        if opcode == 9:
            sock.sendall(ws_frame(10, payload))


class WebSocketHandler(socketserver.BaseRequestHandler):
    """A scripted RFC 6455 server; the request path selects the scenario."""

    def handle(self) -> None:
        try:
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = self.request.recv(4096)
                if not chunk:
                    return
                request += chunk
            head, buffered = request.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            path = lines[0].split(" ")[1]
            headers: dict[str, str] = {}
            for line in lines[1:]:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
            key = headers["sec-websocket-key"]
            accept = base64.b64encode(
                hashlib.sha1((key + WS_GUID).encode()).digest()
            ).decode()
            status = "101 Switching Protocols"
            upgrade = "Upgrade: websocket\r\n"
            connection = "Connection: Upgrade\r\n"
            extra = ""
            if path == "/bad-accept":
                accept = "AAAA"
            if path == "/bad-status":
                status = "200 OK"
            if path == "/bad-upgrade":
                upgrade = ""
            if path == "/bad-connection":
                connection = ""
            if path == "/extensions":
                extra = "Sec-WebSocket-Extensions: permessage-deflate\r\n"
            if path == "/protocol":
                extra = "Sec-WebSocket-Protocol: chat\r\n"
            response = (
                f"HTTP/1.1 {status}\r\n{upgrade}{connection}"
                f"Sec-WebSocket-Accept: {accept}\r\n{extra}\r\n"
            )
            self.request.sendall(response.encode())
            if path in (
                "/bad-accept",
                "/bad-status",
                "/bad-upgrade",
                "/bad-connection",
                "/extensions",
                "/protocol",
            ):
                return
            if path == "/echo":
                while True:
                    opcode, payload, buffered = ws_read_frame(self.request, buffered)
                    if opcode == 8:
                        self.request.sendall(ws_frame(8, payload[:2]))
                        return
                    if opcode == 9:
                        self.request.sendall(ws_frame(10, payload))
                        continue
                    self.request.sendall(ws_frame(opcode, payload))
            elif path == "/fragmented":
                self.request.sendall(ws_frame(1, b"Hello, ", fin=False))
                self.request.sendall(ws_frame(0, b"fragmented ", fin=False))
                self.request.sendall(ws_frame(0, b"world", fin=True))
                ws_read_until_close(self.request, buffered)
            elif path == "/server-ping":
                self.request.sendall(ws_frame(9, b"ka"))
                opcode, payload, buffered = ws_read_frame(self.request, buffered)
                reply = b"pong-ok" if opcode == 10 and payload == b"ka" else b"pong-missing"
                self.request.sendall(ws_frame(1, reply))
                ws_read_until_close(self.request, buffered)
            elif path == "/oversized":
                self.request.sendall(bytes([0x81, 127]) + struct.pack(">Q", 16 * 1024 * 1024 + 1))
                ws_read_until_close(self.request, buffered)
            elif path == "/masked":
                self.request.sendall(ws_masked_frame(1, b"hi"))
                ws_read_until_close(self.request, buffered)
            elif path == "/bad-utf8":
                self.request.sendall(ws_frame(1, b"\xff\xfe"))
                ws_read_until_close(self.request, buffered)
            elif path == "/bad-continuation":
                self.request.sendall(ws_frame(0, b"orphan"))
                ws_read_until_close(self.request, buffered)
            elif path == "/bad-close":
                self.request.sendall(ws_frame(8, struct.pack(">H", 1005)))
                ws_read_until_close(self.request, buffered)
            elif path == "/reserved-bits":
                self.request.sendall(bytes([0x81 | 0x40, 1, 0x41]))
                ws_read_until_close(self.request, buffered)
            elif path == "/close":
                self.request.sendall(ws_frame(8, struct.pack(">H", 1000) + b"bye"))
                try:
                    ws_read_until_close(self.request, buffered)
                except ConnectionError:
                    pass
            elif path == "/silent":
                while self.request.recv(4096):
                    pass
        except (OSError, ConnectionError, KeyError, IndexError):
            pass


def start_ws_server(cert_name: str | None = None) -> QuietTCPServer:
    server = QuietTCPServer(("127.0.0.1", 0), WebSocketHandler)
    if cert_name:
        server.socket = tls_context(cert_name).wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def test_ws_client() -> None:
    print("\nWebSocket client")
    servers: list[QuietTCPServer] = []
    try:
        plain = start_ws_server()
        servers.append(plain)
        secure = start_ws_server("server")
        servers.append(secure)
        run_fixture(
            "ws_client.spark",
            "ws_client.out",
            [
                str(plain.server_address[1]),
                str(secure.server_address[1]),
                str(TLS / "ca.pem"),
            ],
            "websocket handshake, framing, control and close",
        )
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


class MockGatewayHandler(socketserver.BaseRequestHandler):
    """A scripted Discord gateway: HELLO, IDENTIFY, READY, MESSAGE_CREATE.

    It exercises the documented bot example end to end: the client identifies
    with the fixture token, answers the scripted ``ping`` with ``pong`` and
    stops when the gateway closes the connection.
    """

    token = "test-token"

    def _read_json(self, buffered: bytes) -> tuple[dict, bytes]:
        while True:
            opcode, payload, buffered = ws_read_frame(self.request, buffered)
            if opcode == 9:
                self.request.sendall(ws_frame(10, payload))
                continue
            if opcode == 8:
                self.request.sendall(ws_frame(8, payload[:2]))
                raise ConnectionError("client closed")
            if opcode == 1:
                return json.loads(payload), buffered

    def handle(self) -> None:
        try:
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = self.request.recv(4096)
                if not chunk:
                    return
                request += chunk
            head, buffered = request.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            if not lines[0].split(" ")[1].startswith("/?v=10"):
                return
            headers: dict[str, str] = {}
            for line in lines[1:]:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
            key = headers["sec-websocket-key"]
            accept = base64.b64encode(
                hashlib.sha1((key + WS_GUID).encode()).digest()
            ).decode()
            self.request.sendall(
                (
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
                ).encode()
            )
            self.request.settimeout(10)
            hello = {"op": 10, "d": {"heartbeat_interval": 200}}
            self.request.sendall(ws_frame(1, json.dumps(hello).encode()))
            identify, buffered = self._read_json(buffered)
            if identify.get("op") != 2 or identify.get("d", {}).get("token") != self.token:
                self.request.sendall(ws_frame(8, struct.pack(">H", 4004)))
                return
            ready = {"op": 0, "s": 1, "t": "READY", "d": {"session_id": "mock"}}
            self.request.sendall(ws_frame(1, json.dumps(ready).encode()))
            message = {
                "op": 0,
                "s": 2,
                "t": "MESSAGE_CREATE",
                "d": {"content": "ping", "channel_id": "42"},
            }
            self.request.sendall(ws_frame(1, json.dumps(message).encode()))
            reply, buffered = self._read_json(buffered)
            while reply.get("op") == 1:  # heartbeat
                self.request.sendall(ws_frame(11, b"null"))
                reply, buffered = self._read_json(buffered)
            if reply.get("d", {}).get("content") != "pong":
                self.request.sendall(ws_frame(8, struct.pack(">H", 4000)))
                return
            self.request.sendall(ws_frame(8, struct.pack(">H", 1000) + b"done"))
            try:
                opcode, payload, buffered = ws_read_frame(self.request, buffered)
                if opcode == 8:
                    self.request.sendall(ws_frame(8, payload[:2]))
            except (OSError, ConnectionError):
                pass
        except (OSError, ConnectionError, KeyError, IndexError, ValueError):
            pass


def test_discord_example() -> None:
    print("\nDiscord gateway example")
    server = QuietTCPServer(("127.0.0.1", 0), MockGatewayHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        url = f"ws://127.0.0.1:{server.server_address[1]}/?v=10"
        completed = run(
            [str(ROOT / "examples" / "discord_bot.spark")],
            timeout=60,
            env={"DISCORD_TOKEN": "test-token", "DISCORD_GATEWAY_URL": url},
        )
        expected = "ready\nmessage: ping\nreply sent\nclosed\n"
        detail = ""
        ok = completed.returncode == 0 and completed.stdout == expected and not completed.stderr
        if completed.returncode != 0:
            detail += f"exit code {completed.returncode} (expected 0)\n"
        if completed.stdout != expected:
            detail += unified(expected, completed.stdout)
        if completed.stderr:
            detail += "unexpected stderr:\n" + completed.stderr
        report("the Discord example runs against a local mock gateway", ok, detail)
    finally:
        server.shutdown()
        server.server_close()


def test_http_client() -> None:
    print("\nHTTP client")
    server = QuietHTTPServer(("127.0.0.1", 0), ScriptedHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    try:
        completed = run([str(ROOT / "tests" / "http_client.spark"), str(port)], timeout=60)
        expected = (ROOT / "tests" / "http_client.out").read_text(encoding="utf-8")
        detail = ""
        ok = completed.returncode == 0 and completed.stdout == expected and not completed.stderr
        if completed.returncode != 0:
            detail += f"exit code {completed.returncode} (expected 0)\n"
        if completed.stdout != expected:
            detail += unified(expected, completed.stdout)
        if completed.stderr:
            detail += "unexpected stderr:\n" + completed.stderr
        report("http/1.1 client, chunked bodies, redirects and limits", ok, detail)
    finally:
        server.shutdown()
        server.server_close()


def test_http_server() -> None:
    print("\nHTTP server")
    server = subprocess.Popen(
        [str(BINARY), str(ROOT / "tests" / "http_server.spark"), "4"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    try:
        ready, _, _ = select.select([server.stdout], [], [], 20)
        line = server.stdout.readline().strip() if ready else ""
        if not line.startswith("listening "):
            server.kill()
            report("spark http server starts", False, f"the server said {line!r}")
            return
        port = int(line.split(" ", 1)[1])
        completed = run([str(ROOT / "tests" / "http_server_client.spark"), str(port)], timeout=30)
        expected = (ROOT / "tests" / "http_server_client.out").read_text(encoding="utf-8")
        detail = ""
        ok = completed.returncode == 0 and completed.stdout == expected and not completed.stderr
        if completed.returncode != 0:
            detail += f"client exit code {completed.returncode}\n"
        if completed.stdout != expected:
            detail += unified(expected, completed.stdout)
        if completed.stderr:
            detail += "unexpected client stderr:\n" + completed.stderr
        rest, errors = server.communicate(timeout=20)
        if server.returncode != 0 or "served 4" not in rest:
            ok = False
            detail += f"server exit code {server.returncode}, stdout {rest!r}\n{errors}"
        report("spark http server answers spark's http client", ok, detail)
    finally:
        if server.poll() is None:
            server.kill()


def test_examples() -> None:
    print("\nExamples")
    examples = sorted((ROOT / "examples").glob("*.spark"))
    lint_ok = True
    lint_detail = ""
    for program in examples:
        completed = run(["--lint", str(program)])
        if completed.returncode != 0 or completed.stdout or completed.stderr:
            lint_ok = False
            lint_detail += f"{program.name}: {completed.stdout}{completed.stderr}"
    report("the examples are lint-clean", lint_ok, lint_detail)

    server = subprocess.Popen(
        [str(BINARY), str(ROOT / "examples" / "http_server.spark"), "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    try:
        ready, _, _ = select.select([server.stdout], [], [], 20)
        line = server.stdout.readline().strip() if ready else ""
        port = None
        if line.startswith("Serving http://127.0.0.1:"):
            port = int(line.split(":", 2)[2].split("/", 1)[0])
        if port is None:
            report(
                "http_client example talks to the http_server example",
                False,
                f"the server said {line!r}",
            )
        else:
            completed = run(
                [str(ROOT / "examples" / "http_client.spark"), f"http://127.0.0.1:{port}/"],
                timeout=30,
            )
            detail = ""
            ok = (
                completed.returncode == 0
                and "status: 200" in completed.stdout
                and "Hello from Spark!" in completed.stdout
                and not completed.stderr
            )
            if completed.returncode != 0:
                detail += f"exit code {completed.returncode}\n"
            if "status: 200" not in completed.stdout:
                detail += "missing 'status: 200'\n"
            if "Hello from Spark!" not in completed.stdout:
                detail += "missing the response body\n"
            if completed.stderr:
                detail += "unexpected stderr:\n" + completed.stderr
            report("http_client example talks to the http_server example", ok, detail)
    finally:
        if server.poll() is None:
            server.kill()

    server = subprocess.Popen(
        [str(BINARY), str(ROOT / "examples" / "echo_server.spark"), "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    try:
        ready, _, _ = select.select([server.stdout], [], [], 20)
        line = server.stdout.readline().strip() if ready else ""
        port = None
        if line.startswith("listening "):
            port = int(line.split(" ", 1)[1])
        if port is None:
            report("echo_server example echoes a line", False, f"the server said {line!r}")
        else:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as client:
                client.sendall(b"hello\n")
                echoed = client.recv(1024)
            report(
                "echo_server example echoes a line",
                echoed == b"hello\n",
                f"received {echoed!r}",
            )
    finally:
        if server.poll() is None:
            server.kill()


def main() -> int:
    if not BINARY.exists():
        print("missing binary; run 'make' first", file=sys.stderr)
        return 2

    test_cases()
    test_errors()
    test_repl()
    test_error_snippets()
    test_network_limits()
    test_http_client()
    test_http_server()
    test_examples()
    test_tls_client()
    test_https_client()
    test_ws_client()
    test_discord_example()
    test_cli()

    failed = [name for name, ok, _ in results if not ok]
    print()
    print(f"{len(results) - len(failed)} of {len(results)} tests passed")
    if failed:
        print("failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    if "--mock-gateway" in sys.argv:
        gateway = QuietTCPServer(("127.0.0.1", 0), MockGatewayHandler)
        print(f"ws://127.0.0.1:{gateway.server_address[1]}/?v=10", flush=True)
        try:
            gateway.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            gateway.server_close()
        sys.exit(0)
    sys.exit(main())