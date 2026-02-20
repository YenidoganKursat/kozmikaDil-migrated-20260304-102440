#!/usr/bin/env python3
"""Benchmark primitive operators with deterministic random x/y streams.

One task:
- For each primitive/operator pair, generate a single Kozmika program that executes
  `loops` iterations with deterministic pseudo-random inputs and one operator kernel.
- Collect runtime-only timing (build time excluded) and compare baseline vs optimized profiles.

Notes:
- Baseline profile defaults to interpreter mode to represent pre-optimization runtime path.
- Optimized profile defaults to native mode for low/medium precision families.
- f128/f256/f512 are forced to interpreter mode to preserve strict MPFR-backed correctness.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import pathlib
import statistics
import subprocess
import tempfile
import time
from dataclasses import asdict, dataclass
from decimal import Decimal, InvalidOperation, getcontext
from typing import Dict, Iterable, List, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
getcontext().prec = 200

INT_PRIMITIVES = ["i8", "i16", "i32", "i64", "i128", "i256", "i512"]
FLOAT_PRIMITIVES = ["f8", "f16", "bf16", "f32", "f64", "f128", "f256", "f512"]
PRIMITIVES = INT_PRIMITIVES + FLOAT_PRIMITIVES
WIDE_PRIMITIVES = {"i128", "i256", "i512", "f128", "f256", "f512"}
INT_BITS = {
    "i8": 8,
    "i16": 16,
    "i32": 32,
    "i64": 64,
    "i128": 128,
    "i256": 256,
    "i512": 512,
}
OPS: List[Tuple[str, str]] = [
    ("add", "+"),
    ("sub", "-"),
    ("mul", "*"),
    ("div", "/"),
    ("mod", "%"),
    ("pow", "^"),
]


@dataclass
class OpRuntime:
    primitive: str
    op_name: str
    operator: str
    profile: str
    exec_mode: str
    loops: int
    runs: int
    warmup: int
    median_sec: float
    min_sec: float
    max_sec: float
    checksum_raw: str


def is_wide_primitive(primitive: str) -> bool:
    return primitive in WIDE_PRIMITIVES


def hash_int_step_expr(primitive: str) -> List[str]:
    # Integer checksum path:
    # keep deterministic integer state, avoid float drift, and stay overflow-safe
    # under saturating integer semantics.
    return [
        "  tmp_i64 = i64(tmp)",
        "  tmp_mod = tmp_i64 % i64(2147483629)",
        "  if tmp_mod < 0:",
        "    tmp_mod = tmp_mod + i64(2147483629)",
        "  acc = ((acc * i64(48271)) + tmp_mod) % i64(2147483629)",
    ]


def int_safe_abs_for_op(primitive: str, operator: str) -> int:
    # Keep integer benchmark inputs in a range where primitive arithmetic does not
    # overflow for the target operation. This avoids comparing overflow behavior
    # differences between interpreter/native backends.
    bits = INT_BITS.get(primitive, 64)
    effective_bits = max(2, min(bits, 31))
    max_abs = (1 << (effective_bits - 1)) - 1
    if operator == "*":
        bound = int(math.isqrt(max_abs))
    elif operator == "^":
        # Keep integer base small for stable exponentiation.
        bound = min(12, max_abs)
    elif operator in ("+", "-"):
        bound = max_abs // 2
    else:
        bound = max_abs
    return max(1, bound)


def parse_decimal(text: str) -> Decimal | None:
    try:
        return Decimal(text.strip())
    except (InvalidOperation, AttributeError):
        return None


def tolerance_for_primitive(primitive: str, safety_tier: str) -> Decimal:
    if primitive in INT_PRIMITIVES:
        return Decimal("0")
    if primitive == "f8":
        return Decimal("1e-2") if safety_tier == "aggressive" else Decimal("1e-3")
    if primitive in ("f16", "bf16"):
        return Decimal("1e-5") if safety_tier == "aggressive" else Decimal("1e-6")
    if primitive == "f32":
        return Decimal("1e-7")
    if primitive == "f64":
        return Decimal("1e-12")
    if primitive == "f128":
        return Decimal("1e-28")
    if primitive == "f256":
        return Decimal("1e-60")
    if primitive == "f512":
        return Decimal("1e-100")
    return Decimal("1e-12")


def checksum_stats(
    primitive: str,
    baseline_checksum: str,
    optimized_checksum: str,
    safety_tier: str,
) -> Tuple[bool, str, str, str]:
    b = parse_decimal(baseline_checksum)
    o = parse_decimal(optimized_checksum)
    if b is None or o is None:
        # fallback for non-numeric payloads
        return (baseline_checksum == optimized_checksum, "", "", str(tolerance_for_primitive(primitive, safety_tier)))
    if not b.is_finite() or not o.is_finite():
        return (str(b) == str(o), "", "", str(tolerance_for_primitive(primitive, safety_tier)))
    try:
        abs_diff = abs(b - o)
        denom = max(abs(b), abs(o), Decimal("1"))
        rel_diff = abs_diff / denom
    except InvalidOperation:
        return (baseline_checksum == optimized_checksum, "", "", str(tolerance_for_primitive(primitive, safety_tier)))
    tol = tolerance_for_primitive(primitive, safety_tier)
    ok = abs_diff <= tol or rel_diff <= tol
    return (ok, format(abs_diff, "e"), format(rel_diff, "e"), str(tol))


def run_checked(cmd: List[str], env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
        env=merged,
    )


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("empty program output")
    return lines[-1]


def is_float_primitive(primitive: str) -> bool:
    return primitive in FLOAT_PRIMITIVES


def make_program(path: pathlib.Path, primitive: str, operator: str, loops: int, checksum_mode: str) -> None:
    # Deterministic pseudo-random streams:
    # x_n = LCG(seed_x), y_n = LCG(seed_y)
    # Each iteration computes exactly one operator kernel: acc = x <op> y
    # and prints final accumulator after loops.
    lines: List[str] = [
        "seed_x = i64(123456789)",
        "seed_y = i64(362436069)",
        "i = 0",
    ]
    if is_float_primitive(primitive):
        lines.append("acc = f64(0)")
    else:
        lines.append("acc = i64(0)")
    lines += [
        f"while i < {loops}:",
        "  seed_x = (seed_x * 1664525 + 1013904223) % 2147483648",
        "  seed_y = (seed_y * 22695477 + 1) % 2147483648",
    ]
    if is_float_primitive(primitive):
        if operator == "^":
            lines += [
                f"  x = {primitive}((seed_x / 2147483648.0) * 8.0 - 4.0)",
                "  exp_raw = (seed_y % 9) - 4",
                f"  y = {primitive}(exp_raw)",
                "  if x == 0 and y < 0:",
                f"    y = {primitive}(1)",
            ]
        else:
            lines += [
                f"  x = {primitive}((seed_x / 2147483648.0) * 200.0 - 100.0)",
                f"  y = {primitive}((seed_y / 2147483648.0) * 200.0 - 100.0)",
            ]
    else:
        int_abs = int_safe_abs_for_op(primitive, operator)
        span = int_abs * 2 + 1
        if operator == "^":
            lines += [
                f"  x_raw = (seed_x % {span}) - {int_abs}",
                "  y_raw = seed_y % 6",
                f"  x = {primitive}(x_raw)",
                f"  y = {primitive}(y_raw)",
            ]
        else:
            lines += [
                f"  x_raw = (seed_x % {span}) - {int_abs}",
                f"  y_raw = (seed_y % {span}) - {int_abs}",
                f"  x = {primitive}(x_raw)",
                f"  y = {primitive}(y_raw)",
            ]
    if operator in ("/", "%"):
        if is_float_primitive(primitive):
            lines += [
                "  if y == 0:",
                f"    y = {primitive}(0.5)",
            ]
        else:
            lines += [
                "  if y == 0:",
                f"    y = {primitive}(1)",
            ]
    lines += [
        f"  tmp = x {operator} y",
    ]
    if checksum_mode == "accumulate":
        if is_float_primitive(primitive):
            lines += [
                "  acc = acc + f64(tmp)",
            ]
        else:
            lines += hash_int_step_expr(primitive)
    else:
        if is_float_primitive(primitive):
            lines += [
                "  acc = f64(tmp)",
            ]
        else:
            lines += [
                "  acc = i64(tmp)",
            ]
    lines += [
        "  i = i + 1",
        "print(acc)",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def build_native(program: pathlib.Path, binary: pathlib.Path, env: Dict[str, str]) -> None:
    run_checked(["./k", "build", str(program), "-o", str(binary)], env=env)


def run_profile_once(
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    runs: int,
    warmup: int,
    profile: str,
    exec_mode: str,
    checksum_mode: str,
    env: Dict[str, str],
    safety_tier: str,
) -> OpRuntime:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-prim-{primitive}-{op_name}-{profile}-") as tmp:
        tmpdir = pathlib.Path(tmp)
        program = tmpdir / f"{primitive}_{op_name}.k"
        binary = tmpdir / f"{primitive}_{op_name}.bin"
        make_program(program, primitive, operator, loops, checksum_mode)

        resolved_mode = exec_mode
        if primitive in ("f128", "f256", "f512"):
            # Strict correctness: high-precision float families must run via interpreter/MPFR.
            resolved_mode = "interpret"
        if profile == "optimized" and safety_tier in ("strict", "hybrid") and is_wide_primitive(primitive):
            resolved_mode = "interpret"
        if resolved_mode != "interpret":
            try:
                build_native(program, binary, env)
                resolved_mode = "native"
            except subprocess.CalledProcessError:
                if exec_mode == "native":
                    raise
                resolved_mode = "interpret"

        for _ in range(warmup):
            if resolved_mode == "native":
                run_checked([str(binary)], env=env)
            else:
                run_checked(["./k", "run", "--interpret", str(program)], env=env)

        samples: List[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            if resolved_mode == "native":
                proc = run_checked([str(binary)], env=env)
            else:
                proc = run_checked(["./k", "run", "--interpret", str(program)], env=env)
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    return OpRuntime(
        primitive=primitive,
        op_name=op_name,
        operator=operator,
        profile=profile,
        exec_mode=resolved_mode,
        loops=loops,
        runs=runs,
        warmup=warmup,
        median_sec=statistics.median(samples),
        min_sec=min(samples),
        max_sec=max(samples),
        checksum_raw=checksum,
    )


def default_env_for_profile(profile: str, safety_tier: str, primitive: str) -> Dict[str, str]:
    if profile == "baseline":
        return {
            "SPARK_CFLAGS": "-std=c11 -O2 -DNDEBUG",
            "SPARK_LTO": "off",
            "SPARK_ALLOW_APPROX_HIGH_PRECISION_NATIVE": "0",
        }
    if profile == "optimized":
        if safety_tier == "aggressive":
            return {
                "SPARK_CFLAGS": "-std=c11 -Ofast -DNDEBUG -march=native -mtune=native "
                                "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                                "-fno-math-errno -fno-trapping-math -ffp-contract=fast",
                "SPARK_LTO": "full",
                "SPARK_ALLOW_APPROX_HIGH_PRECISION_NATIVE": "0",
            }
        if safety_tier == "hybrid":
            # Hybrid: strict semantics for wide/high-precision primitives via interpreter fallback,
            # native fast path for low-width primitives with safe optimization flags.
            allow_approx = "0"
            if primitive in ("f128", "f256", "f512"):
                allow_approx = "0"
            return {
                "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                                "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                                "-fno-math-errno -fno-trapping-math",
                "SPARK_LTO": "thin",
                "SPARK_ALLOW_APPROX_HIGH_PRECISION_NATIVE": allow_approx,
            }
        if safety_tier == "strict":
            return {
                "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -fstrict-aliasing -fno-math-errno "
                                "-fno-trapping-math",
                "SPARK_LTO": "off",
                "SPARK_ALLOW_APPROX_HIGH_PRECISION_NATIVE": "0",
            }
    raise ValueError(f"unknown profile: {profile}")


def parse_filter(raw: str, allowed: Iterable[str]) -> List[str]:
    if not raw:
        return list(allowed)
    selected = [part.strip() for part in raw.split(",") if part.strip()]
    unknown = [part for part in selected if part not in allowed]
    if unknown:
        raise SystemExit(f"unknown filter values: {','.join(unknown)}")
    return selected


def write_csv(path: pathlib.Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark all primitive operators with random x/y streams")
    parser.add_argument("--loops", type=int, default=100_000_000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--primitives", type=str, default="")
    parser.add_argument("--ops", type=str, default="")
    parser.add_argument(
        "--checksum-mode",
        choices=["accumulate", "last"],
        default="accumulate",
        help="`accumulate` keeps per-iteration dependency to avoid dead-code elimination.",
    )
    parser.add_argument(
        "--out-prefix",
        type=str,
        default="primitive_ops_random_benchmark",
        help="Result file prefix under bench/results/primitives.",
    )
    parser.add_argument(
        "--baseline-exec",
        choices=["interpret", "native", "auto"],
        default="interpret",
        help="Baseline execution mode. Default interpret to represent pre-optimization runtime.",
    )
    parser.add_argument(
        "--optimized-exec",
        choices=["interpret", "native", "auto"],
        default="native",
        help="Optimized execution mode. Default native for runtime acceleration.",
    )
    parser.add_argument(
        "--safety-tier",
        choices=["strict", "hybrid", "aggressive"],
        default="hybrid",
        help="strict: correctness-first, hybrid: wide types strict + low types fast, aggressive: max throughput.",
    )
    parser.add_argument(
        "--fail-on-mismatch",
        action="store_true",
        help="Exit non-zero if tolerant checksum policy fails for any primitive/operator row.",
    )
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    selected_primitives = parse_filter(args.primitives, PRIMITIVES)
    selected_ops = parse_filter(args.ops, [name for name, _ in OPS])

    ops_by_name = {name: symbol for name, symbol in OPS}

    baseline_records: Dict[Tuple[str, str], OpRuntime] = {}
    optimized_records: Dict[Tuple[str, str], OpRuntime] = {}

    for primitive in selected_primitives:
        for op_name in selected_ops:
            operator = ops_by_name[op_name]
            baseline_env = default_env_for_profile("baseline", args.safety_tier, primitive)
            optimized_env = default_env_for_profile("optimized", args.safety_tier, primitive)
            baseline = run_profile_once(
                primitive=primitive,
                op_name=op_name,
                operator=operator,
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                profile="baseline",
                exec_mode=args.baseline_exec,
                checksum_mode=args.checksum_mode,
                env=baseline_env,
                safety_tier=args.safety_tier,
            )
            optimized = run_profile_once(
                primitive=primitive,
                op_name=op_name,
                operator=operator,
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                profile="optimized",
                exec_mode=args.optimized_exec,
                checksum_mode=args.checksum_mode,
                env=optimized_env,
                safety_tier=args.safety_tier,
            )
            baseline_records[(primitive, op_name)] = baseline
            optimized_records[(primitive, op_name)] = optimized

            speedup = (
                baseline.median_sec / optimized.median_sec
                if optimized.median_sec > 0.0 else 0.0
            )
            print(
                f"{primitive:<5} {operator:<1} "
                f"base={baseline.median_sec:.6f}s({baseline.exec_mode}) "
                f"opt={optimized.median_sec:.6f}s({optimized.exec_mode}) "
                f"speedup={speedup:.3f}x"
            , flush=True)

    rows: List[Dict[str, object]] = []
    for primitive in selected_primitives:
        for op_name in selected_ops:
            base = baseline_records[(primitive, op_name)]
            opt = optimized_records[(primitive, op_name)]
            speedup = base.median_sec / opt.median_sec if opt.median_sec > 0.0 else 0.0
            checksums_match_tolerant, checksum_abs_diff, checksum_rel_diff, checksum_tolerance = checksum_stats(
                primitive=primitive,
                baseline_checksum=base.checksum_raw,
                optimized_checksum=opt.checksum_raw,
                safety_tier=args.safety_tier,
            )
            rows.append(
                {
                    "primitive": primitive,
                    "op_name": op_name,
                    "operator": ops_by_name[op_name],
                    "loops": args.loops,
                    "baseline_mode": base.exec_mode,
                    "baseline_median_sec": base.median_sec,
                    "optimized_mode": opt.exec_mode,
                    "optimized_median_sec": opt.median_sec,
                    "speedup_vs_baseline": speedup,
                    "baseline_checksum": base.checksum_raw,
                    "optimized_checksum": opt.checksum_raw,
                    "checksums_equal": str(base.checksum_raw == opt.checksum_raw),
                    "checksums_match_tolerant": str(checksums_match_tolerant),
                    "checksum_abs_diff": checksum_abs_diff,
                    "checksum_rel_diff": checksum_rel_diff,
                    "checksum_tolerance": checksum_tolerance,
                }
            )

    out_json = RESULT_DIR / f"{args.out_prefix}.json"
    out_csv = RESULT_DIR / f"{args.out_prefix}.csv"
    payload = {
        "loops": args.loops,
        "runs": args.runs,
        "warmup": args.warmup,
        "baseline_exec": args.baseline_exec,
        "optimized_exec": args.optimized_exec,
        "safety_tier": args.safety_tier,
        "checksum_mode": args.checksum_mode,
        "records": rows,
    }
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    write_csv(out_csv, rows)
    print(f"result_json: {out_json}")
    print(f"result_csv: {out_csv}")
    if args.fail_on_mismatch:
        mismatches = [row for row in rows if row["checksums_match_tolerant"] != "True"]
        if mismatches:
            print(f"checksum_mismatch_rows: {len(mismatches)}")
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
