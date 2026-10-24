#!/usr/bin/env python3
"""Run the Fibonacci benchmark several times and summarize the timings."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "chispa"
SCRIPT = ROOT / "benchmarks" / "fib.spark"
RUNS = 7


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--max-ms",
        type=float,
        default=None,
        help="fail when the minimum time is above this many milliseconds",
    )
    options = parser.parse_args()

    times = []
    result = ""
    for _ in range(RUNS):
        completed = subprocess.run(
            [str(BINARY), str(SCRIPT)], capture_output=True, text=True, encoding="utf-8"
        )
        if completed.returncode != 0:
            print(completed.stderr, file=sys.stderr)
            return 1
        lines = completed.stdout.strip().splitlines()
        if not lines:
            print("error: the benchmark produced no output", file=sys.stderr)
            return 1
        result = lines[0]
        times.append(float(lines[-1]))

    minimum = min(times)
    print(f"result: {result}")
    print("times (ms): " + " ".join(f"{t:.2f}" for t in times))
    print(f"minimum: {minimum:.2f} ms")
    print(f"median: {statistics.median(times):.2f} ms")

    if options.max_ms is not None and minimum > options.max_ms:
        print(
            f"error: minimum {minimum:.2f} ms is above the {options.max_ms:.2f} ms budget",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
