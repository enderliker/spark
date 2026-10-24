#!/usr/bin/env python3
"""Run all Spark benchmarks and display results."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPARK = ROOT / "spark"
BENCH_DIR = ROOT / "benchmarks"

BENCHMARKS = [
    ("fib(27)", BENCH_DIR / "fib.spark"),
    ("int_loop (5M)", BENCH_DIR / "int_loop.spark"),
    ("float_loop (3M)", BENCH_DIR / "float_loop.spark"),
    ("closures (1M)", BENCH_DIR / "closures.spark"),
    ("higher_order (300k)", BENCH_DIR / "higher_order.spark"),
    ("arrays (1M)", BENCH_DIR / "arrays.spark"),
    ("alloc (1M)", BENCH_DIR / "alloc.spark"),
    ("strings (200k)", BENCH_DIR / "strings.spark"),
    ("maps (200k)", BENCH_DIR / "maps.spark"),
    ("sort (100k)", BENCH_DIR / "sort.spark"),
    ("sort_by (100k)", BENCH_DIR / "sort_by.spark"),
]

RUNS = 7


def run_benchmark(name: str, path: Path):
    times = []
    for _ in range(RUNS):
        proc = subprocess.run([str(SPARK), str(path)], capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"Error running {name}: {proc.stderr}", file=sys.stderr)
            return None
        lines = proc.stdout.strip().splitlines()
        try:
            ms = float(lines[-1])
            times.append(ms)
        except (ValueError, IndexError):
            print(f"Bad output for {name}: {proc.stdout}", file=sys.stderr)
            return None
    times.sort()
    min_ms = times[0]
    med_ms = times[len(times) // 2]
    return min_ms, med_ms


def main():
    print(f"{'Benchmark':<24} | {'Min (ms)':<10} | {'Median (ms)':<10}")
    print("-" * 50)
    for name, path in BENCHMARKS:
        res = run_benchmark(name, path)
        if res:
            min_ms, med_ms = res
            print(f"{name:<24} | {min_ms:<10.2f} | {med_ms:<10.2f}")


if __name__ == "__main__":
    main()
