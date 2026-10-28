#!/usr/bin/env python3
"""Automated test suite for the Spark language."""

from __future__ import annotations

import difflib
import select
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

results: list[tuple[str, bool, str]] = []


def report(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(("  ok     " if ok else "  FAIL   ") + name)
    if not ok and detail:
        for line in detail.rstrip().splitlines():
            print("         " + line)


def run(args: list[str], stdin: str | None = None, timeout: int = 60):
    return subprocess.run(
        [str(BINARY), *args],
        input=stdin,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
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
    test_cli()

    failed = [name for name, ok, _ in results if not ok]
    print()
    print(f"{len(results) - len(failed)} of {len(results)} tests passed")
    if failed:
        print("failed: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())