#!/usr/bin/env python3
"""Single-task benchmark wrapper for f64 scalar primitive."""

from benchmark_primitive_scalar_core import run_benchmark

if __name__ == "__main__":
    result = run_benchmark("f64", loops=200000, warmup=1, runs=3)
    print(f"{result.primitive}: median={result.median_sec:.6f}s baseline_1x={result.baseline_1x_sec:.6f}s speedup={result.speedup_vs_1x:.3f}x checksum={result.checksum_raw}")
