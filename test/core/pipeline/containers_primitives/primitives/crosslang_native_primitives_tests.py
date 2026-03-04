#!/usr/bin/env python3
"""Phase-5 cross-language native primitive correctness test runner.

Runs:
1) Integer primitive validation vs Python model (+ extreme vectors).
2) Float stepwise validation vs Java BigDecimal (+ optional Python cross-check).
3) Float extreme-vector validation vs Java BigDecimal + Python Decimal.
"""

from __future__ import annotations

import argparse
import pathlib
import random
import shutil
import subprocess
import sys
import time
from typing import List

REPO_ROOT = pathlib.Path(__file__).resolve().parents[5]


def run_cmd(cmd: List[str]) -> None:
    subprocess.run(cmd, cwd=str(REPO_ROOT), check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run cross-language primitive unit tests")
    parser.add_argument("--int-loops", type=int, default=80000)
    parser.add_argument("--int-extreme-random", type=int, default=96)
    parser.add_argument("--int-bigint-init-random", type=int, default=24)
    parser.add_argument("--int-bigint-op-random", type=int, default=32)
    parser.add_argument("--float-loops", type=int, default=1500)
    parser.add_argument("--float-python-loops", type=int, default=1500)
    parser.add_argument("--float-extreme-random", type=int, default=48)
    parser.add_argument("--all-series-perf-loops", type=int, default=20000)
    parser.add_argument("--all-series-perf-runs", type=int, default=1)
    parser.add_argument("--all-series-perf-warmup", type=int, default=0)
    parser.add_argument(
        "--all-series-perf-mode",
        choices=["auto", "interpret", "native"],
        default="interpret",
    )
    parser.add_argument(
        "--skip-all-series-perf",
        action="store_true",
        help="Skip all f/i series primitive-cycle perf benchmark.",
    )
    parser.add_argument("--single-op-loops", type=int, default=200000)
    parser.add_argument("--single-op-runs", type=int, default=3)
    parser.add_argument(
        "--randomize-seed",
        action="store_true",
        help="Generate a fresh random seed for this test run and feed it to validators.",
    )
    parser.add_argument(
        "--random-seed",
        type=int,
        default=None,
        help="Explicit shared seed for validator random generators.",
    )
    parser.add_argument(
        "--allow-missing-java",
        action="store_true",
        help="Skip Java-backed checks when java/javac are unavailable.",
    )
    args = parser.parse_args()

    shared_seed = args.random_seed
    if shared_seed is None and args.randomize_seed:
        shared_seed = random.SystemRandom().randrange(1, 2**31)
    seed_args: List[str] = []
    if shared_seed is not None:
        print(f"crosslang_random_seed={shared_seed}")
        seed_args = ["--random-seed", str(shared_seed)]

    java_ok = shutil.which("java") is not None and shutil.which("javac") is not None
    if not java_ok and not args.allow_missing_java:
        print("java/javac not found; Java-backed cross-language checks are required.")
        return 1

    start = time.perf_counter()

    run_cmd(
        [
            sys.executable,
            "bench/scripts/primitives/validate_int_ops_python.py",
            "--loops",
            str(args.int_loops),
            "--include-extremes",
            "--extreme-random-cases",
            str(args.int_extreme_random),
            "--fail-on-mismatch",
            *seed_args,
        ]
    )
    if java_ok:
        run_cmd(
            [
                sys.executable,
                "bench/scripts/primitives/validate_int_init_ops_java_bigint.py",
                "--random-init-cases",
                str(args.int_bigint_init_random),
                "--random-op-cases",
                str(args.int_bigint_op_random),
                "--fail-on-mismatch",
                *seed_args,
            ]
        )

    if java_ok:
        run_cmd(
            [
                sys.executable,
                "bench/scripts/primitives/check_float_ops_stepwise_bigdecimal.py",
                "--loops",
                str(args.float_loops),
                "--python-crosscheck",
                "--python-loops",
                str(args.float_python_loops),
                "--fail-on-nonfinite",
            ]
        )
        run_cmd(
            [
                sys.executable,
                "bench/scripts/primitives/validate_float_extreme_bigdecimal.py",
                "--random-cases",
                str(args.float_extreme_random),
                "--fail-on-mismatch",
                *seed_args,
            ]
        )
    else:
        print("java/javac not found -> Java-backed float checks skipped (allow-missing-java enabled).")

    # Single-op smoke benchmark: verifies the c=a op b micro-window path remains runnable.
    single_op_languages = ["kozmika-native", "c", "cpp"]
    if shutil.which("dotnet") is not None:
        single_op_languages.append("csharp")
    run_cmd(
        [
            sys.executable,
            "bench/scripts/primitives/benchmark_single_op_window_crosslang.py",
            "--primitive",
            "f64",
            "--operator",
            "+",
            "--loops",
            str(args.single_op_loops),
            "--runs",
            str(args.single_op_runs),
            "--languages",
            ",".join(single_op_languages),
        ]
    )

    if not args.skip_all_series_perf:
        run_cmd(
            [
                sys.executable,
                "bench/scripts/primitives/benchmark_all_primitives_cycle.py",
                "--loops",
                str(args.all_series_perf_loops),
                "--runs",
                str(args.all_series_perf_runs),
                "--warmup",
                str(args.all_series_perf_warmup),
                "--mode",
                args.all_series_perf_mode,
            ]
        )

    elapsed = time.perf_counter() - start
    print(f"crosslang_primitive_tests_elapsed_sec={elapsed:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
