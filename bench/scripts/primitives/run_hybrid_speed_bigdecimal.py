#!/usr/bin/env python3
"""Run hybrid primitive speed sweep + BigDecimal validation in one command."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
from statistics import median
from typing import Dict, List, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"
BENCH_SCRIPT = REPO_ROOT / "bench" / "scripts" / "primitives" / "benchmark_primitive_ops_random.py"
BIGDEC_SCRIPT = REPO_ROOT / "bench" / "scripts" / "primitives" / "check_float_ops_stepwise_bigdecimal.py"

# Absolute-error targets used for report gating.
# These are intentionally conservative for low-precision families and strict for high precision.
ABS_TARGET = {
    "f8": 1.25e-1,
    "f16": 9.766e-4,
    "bf16": 7.8125e-3,
    "f32": 1.0e-7,
    "f64": 1.0e-12,
    "f128": 1.0e-28,
    "f256": 1.0e-60,
    "f512": 1.0e-100,
}


def run_checked(cmd: List[str]) -> None:
    subprocess.run(cmd, cwd=str(REPO_ROOT), check=True)


def run_bench(
    loops: int,
    runs: int,
    warmup: int,
    safety_tier: str,
    out_prefix: str,
    primitives: str,
    ops: str,
    fail_on_mismatch: bool,
) -> pathlib.Path:
    cmd = [
        sys.executable,
        str(BENCH_SCRIPT),
        "--loops",
        str(loops),
        "--runs",
        str(runs),
        "--warmup",
        str(warmup),
        "--checksum-mode",
        "accumulate",
        "--baseline-exec",
        "interpret",
        "--optimized-exec",
        "auto",
        "--safety-tier",
        safety_tier,
        "--out-prefix",
        out_prefix,
    ]
    if fail_on_mismatch:
        cmd.append("--fail-on-mismatch")
    if primitives:
        cmd.extend(["--primitives", primitives])
    if ops:
        cmd.extend(["--ops", ops])
    run_checked(cmd)
    return RESULT_DIR / f"{out_prefix}.json"


def row_key(row: Dict[str, object]) -> Tuple[str, str]:
    return (str(row.get("primitive", "")), str(row.get("op_name", "")))


def main() -> int:
    parser = argparse.ArgumentParser(description="Hybrid speed + BigDecimal validation pipeline")
    parser.add_argument("--loops", type=int, default=2_000_000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--bigdecimal-loops", type=int, default=20_000)
    parser.add_argument("--python-loops", type=int, default=20_000)
    parser.add_argument("--safety-tier", choices=["strict", "hybrid"], default="hybrid")
    parser.add_argument(
        "--strategy",
        choices=["single", "layered"],
        default="single",
        help="single: run one tier; layered: run strict+hybrid and choose faster row when checksum passes.",
    )
    parser.add_argument("--out-prefix", type=str, default="hybrid_speed_bigdecimal")
    parser.add_argument("--primitives", type=str, default="")
    parser.add_argument("--ops", type=str, default="")
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    bench_prefix = f"{args.out_prefix}_bench"
    bench_json: pathlib.Path
    bench_rows: List[Dict[str, object]]
    selected_by_tier: Dict[str, int] = {}

    if args.strategy == "single":
        bench_json = run_bench(
            loops=args.loops,
            runs=args.runs,
            warmup=args.warmup,
            safety_tier=args.safety_tier,
            out_prefix=bench_prefix,
            primitives=args.primitives,
            ops=args.ops,
            fail_on_mismatch=True,
        )
        bench_payload = json.loads(bench_json.read_text(encoding="utf-8"))
        bench_rows = list(bench_payload.get("records", []))
        selected_by_tier = {args.safety_tier: len(bench_rows)}
    else:
        strict_prefix = f"{args.out_prefix}_bench_strict"
        hybrid_prefix = f"{args.out_prefix}_bench_hybrid"
        strict_json = run_bench(
            loops=args.loops,
            runs=args.runs,
            warmup=args.warmup,
            safety_tier="strict",
            out_prefix=strict_prefix,
            primitives=args.primitives,
            ops=args.ops,
            fail_on_mismatch=True,
        )
        hybrid_json = run_bench(
            loops=args.loops,
            runs=args.runs,
            warmup=args.warmup,
            safety_tier="hybrid",
            out_prefix=hybrid_prefix,
            primitives=args.primitives,
            ops=args.ops,
            fail_on_mismatch=True,
        )
        strict_payload = json.loads(strict_json.read_text(encoding="utf-8"))
        hybrid_payload = json.loads(hybrid_json.read_text(encoding="utf-8"))
        strict_rows = list(strict_payload.get("records", []))
        hybrid_rows = list(hybrid_payload.get("records", []))
        strict_map = {row_key(row): row for row in strict_rows}
        hybrid_map = {row_key(row): row for row in hybrid_rows}

        merged: List[Dict[str, object]] = []
        selected_by_tier = {"strict": 0, "hybrid": 0}
        for key, s_row in strict_map.items():
            best_row = dict(s_row)
            best_row["source_tier"] = "strict"
            h_row = hybrid_map.get(key)
            if h_row is not None:
                h_ok = str(h_row.get("checksums_match_tolerant")) == "True"
                h_speed = float(h_row.get("speedup_vs_baseline", 0.0))
                s_speed = float(s_row.get("speedup_vs_baseline", 0.0))
                if h_ok and h_speed > s_speed:
                    best_row = dict(h_row)
                    best_row["source_tier"] = "hybrid"
            selected_by_tier[str(best_row["source_tier"])] += 1
            merged.append(best_row)

        bench_payload = {
            "strategy": "layered",
            "loops": args.loops,
            "runs": args.runs,
            "warmup": args.warmup,
            "baseline_exec": "interpret",
            "optimized_exec": "auto",
            "records": merged,
            "source": {
                "strict_json": str(strict_json),
                "hybrid_json": str(hybrid_json),
            },
        }
        bench_json = RESULT_DIR / f"{bench_prefix}.json"
        bench_json.write_text(json.dumps(bench_payload, indent=2), encoding="utf-8")
        bench_rows = merged

    run_checked(
        [
            sys.executable,
            str(BIGDEC_SCRIPT),
            "--loops",
            str(args.bigdecimal_loops),
            "--python-crosscheck",
            "--python-loops",
            str(args.python_loops),
        ]
    )

    bigdec_json = RESULT_DIR / "float_ops_stepwise_bigdecimal_check.json"
    bigdec_payload = json.loads(bigdec_json.read_text(encoding="utf-8"))

    speedups = [float(row["speedup_vs_baseline"]) for row in bench_rows]
    tolerant_mismatch = [row for row in bench_rows if str(row.get("checksums_match_tolerant")) != "True"]
    speed_ge_1000 = [row for row in bench_rows if float(row["speedup_vs_baseline"]) >= 1000.0]

    bigdec_rows = bigdec_payload.get("rows", [])
    bigdec_fails = []
    for row in bigdec_rows:
        primitive = row["primitive"]
        max_abs = float(row.get("max_abs_error_vs_bigdecimal", 0.0))
        non_finite = int(row.get("non_finite_count", 0))
        limit = ABS_TARGET.get(primitive, 1e-12)
        if non_finite > 0 or max_abs > limit:
            bigdec_fails.append(
                {
                    "primitive": primitive,
                    "operator": row["operator"],
                    "max_abs_error_vs_bigdecimal": max_abs,
                    "abs_target": limit,
                    "non_finite_count": non_finite,
                }
            )

    summary = {
        "safety_tier": args.safety_tier,
        "loops": args.loops,
        "runs": args.runs,
        "warmup": args.warmup,
        "bigdecimal_loops": args.bigdecimal_loops,
        "python_loops": args.python_loops,
        "strategy": args.strategy,
        "speedup_min": min(speedups) if speedups else 0.0,
        "speedup_median": median(speedups) if speedups else 0.0,
        "speedup_max": max(speedups) if speedups else 0.0,
        "speed_rows_total": len(bench_rows),
        "speed_rows_ge_1000x": len(speed_ge_1000),
        "tolerant_checksum_mismatch_rows": len(tolerant_mismatch),
        "bigdecimal_rows_total": len(bigdec_rows),
        "bigdecimal_fail_rows": len(bigdec_fails),
        "artifacts": {
            "bench_json": str(bench_json),
            "bigdecimal_json": str(bigdec_json),
        },
        "examples_1000x": speed_ge_1000[:10],
        "checksum_mismatches": tolerant_mismatch[:20],
        "bigdecimal_fail_examples": bigdec_fails[:20],
        "selected_by_tier": selected_by_tier,
    }

    report_json = RESULT_DIR / f"{args.out_prefix}_report.json"
    report_md = RESULT_DIR / f"{args.out_prefix}_report.md"
    report_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    md_lines = [
        "# Hybrid Speed + BigDecimal Report",
        "",
        f"- strategy: `{args.strategy}`",
        f"- safety_tier: `{args.safety_tier}`",
        f"- speedup min/median/max: `{summary['speedup_min']:.3f}x / {summary['speedup_median']:.3f}x / {summary['speedup_max']:.3f}x`",
        f"- rows >= 1000x: `{summary['speed_rows_ge_1000x']}/{summary['speed_rows_total']}`",
        f"- tolerant checksum mismatch rows: `{summary['tolerant_checksum_mismatch_rows']}`",
        f"- BigDecimal fail rows: `{summary['bigdecimal_fail_rows']}/{summary['bigdecimal_rows_total']}`",
        f"- selected_by_tier: `{selected_by_tier}`",
        "",
        f"- bench_json: `{bench_json}`",
        f"- bigdecimal_json: `{bigdec_json}`",
        f"- report_json: `{report_json}`",
    ]
    report_md.write_text("\n".join(md_lines) + "\n", encoding="utf-8")

    print(f"report_json: {report_json}")
    print(f"report_md: {report_md}")
    print(
        "summary:",
        f"min={summary['speedup_min']:.3f}x",
        f"median={summary['speedup_median']:.3f}x",
        f"max={summary['speedup_max']:.3f}x",
        f"rows_ge_1000x={summary['speed_rows_ge_1000x']}",
        f"checksum_mismatch={summary['tolerant_checksum_mismatch_rows']}",
        f"bigdecimal_fail={summary['bigdecimal_fail_rows']}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
