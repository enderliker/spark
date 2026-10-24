#!/usr/bin/env python3
"""Automated test suite for the Chispa language."""

from __future__ import annotations

import difflib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "chispa"
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
        "prints the result of an expression": "chispa> 3" in completed.stdout,
        "keeps global variables": "chispa> 10" in completed.stdout,
        "accepts multi-line blocks": "chispa> 42" in completed.stdout,
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


def test_cli() -> None:
    print("\nCommand-line interface")
    version = run(["--version"])
    report("--version", version.returncode == 0 and "Chispa" in version.stdout,
           version.stdout + version.stderr)

    help_run = run(["--help"])
    report("--help", help_run.returncode == 0 and "Usage: chispa" in help_run.stdout,
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


def main() -> int:
    if not BINARY.exists():
        print("missing binary; run 'make' first", file=sys.stderr)
        return 2

    test_cases()
    test_errors()
    test_repl()
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