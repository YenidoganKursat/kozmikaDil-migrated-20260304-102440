"""CLI controller for random primitive operator benchmark."""

from __future__ import annotations

import argparse
import json
from decimal import getcontext

from .checksum import checksum_stats
from .constants import OPS, PRIMITIVES
from .io_utils import RESULT_DIR, parse_filter, write_csv
from .runtime import default_env_for_profile, run_profile_once

getcontext().prec = 200


def build_parser() -> argparse.ArgumentParser:
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
        choices=["strict", "hybrid"],
        default="hybrid",
        help="strict: correctness-first, hybrid: wide types strict + low types fast.",
    )
    parser.add_argument(
        "--fail-on-mismatch",
        action="store_true",
        help="Exit non-zero if tolerant checksum policy fails for any primitive/operator row.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    selected_primitives = parse_filter(args.primitives, PRIMITIVES)
    selected_ops = parse_filter(args.ops, [name for name, _ in OPS])
    ops_by_name = {name: symbol for name, symbol in OPS}

    baseline_records = {}
    optimized_records = {}

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
            speedup = baseline.median_sec / optimized.median_sec if optimized.median_sec > 0.0 else 0.0
            print(
                f"{primitive:<5} {operator:<1} "
                f"base={baseline.median_sec:.6f}s({baseline.exec_mode}) "
                f"opt={optimized.median_sec:.6f}s({optimized.exec_mode}) "
                f"speedup={speedup:.3f}x",
                flush=True,
            )

    rows: list[dict[str, object]] = []
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

