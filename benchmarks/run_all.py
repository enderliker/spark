#!/usr/bin/env python3
"""Run all Spark benchmarks and display results."""

import socketserver
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPARK = ROOT / "spark"
BENCH_DIR = ROOT / "benchmarks"

BENCHMARKS = [
    ("fib(27)", BENCH_DIR / "fib.spark"),
    ("echo (2k round-trips)", BENCH_DIR / "echo.spark"),
    ("http (200 round-trips)", BENCH_DIR / "http_roundtrip.spark", ["{http_port}"]),
    ("int_loop (5M)", BENCH_DIR / "int_loop.spark"),
    ("float_loop (3M)", BENCH_DIR / "float_loop.spark"),
    ("closures (1M)", BENCH_DIR / "closures.spark"),
    ("higher_order (300k)", BENCH_DIR / "higher_order.spark"),
    ("arrays (1M)", BENCH_DIR / "arrays.spark"),
    ("alloc (1M)", BENCH_DIR / "alloc.spark"),
    ("strings (200k)", BENCH_DIR / "strings.spark"),
    ("string_builder (200k)", BENCH_DIR / "string_build.spark"),
    ("maps (200k)", BENCH_DIR / "maps.spark"),
    ("sort (100k)", BENCH_DIR / "sort.spark"),
    ("sort_by (100k)", BENCH_DIR / "sort_by.spark"),
]

RUNS = 7


class LocalHTTPHandler(socketserver.BaseRequestHandler):
    """Answers every request with a fixed small response."""

    def handle(self):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            data += chunk
        self.request.sendall(
            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"
        )


class LocalHTTPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def run_benchmark(name: str, path: Path, args=(), http_port=None):
    times = []
    command = [str(SPARK), str(path)]
    command.extend(arg.format(http_port=http_port) for arg in args)
    for _ in range(RUNS):
        proc = subprocess.run(command, capture_output=True, text=True)
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
    server = LocalHTTPServer(("127.0.0.1", 0), LocalHTTPHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for entry in BENCHMARKS:
            name, path, *rest = entry
            args = rest[0] if rest else ()
            res = run_benchmark(name, path, args, server.server_address[1])
            if res:
                min_ms, med_ms = res
                print(f"{name:<24} | {min_ms:<10.2f} | {med_ms:<10.2f}")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
