"""CLI controller for single-op cross-language window benchmark."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict

from .c_family_runner import run_c_like
from .common import RESULT_DIR
from .csharp_runner import run_csharp
from .kozmika_runner import run_kozmika
from .models import WindowResult


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Benchmark single-op now->op->now window across languages")
    parser.add_argument("--primitive", default="f64", help="Kozmika primitive (default: f64)")
    parser.add_argument("--operator", default="+", choices=["+", "-", "*", "/", "%", "^"])
    parser.add_argument("--loops", type=int, default=200_000)
    parser.add_argument("--batch", type=int, default=1)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--a", default="1.000123456789")
    parser.add_argument("--b", default="1.000000000001")
    parser.add_argument(
        "--languages",
        default="kozmika-interpret,kozmika-native,c,cpp,csharp",
        help="comma-separated: kozmika-interpret,kozmika-native,c,cpp,csharp",
    )
    parser.add_argument(
        "--kozmika-tick-mode",
        choices=["ns", "raw"],
        default="raw",
        help="bench tick source for Kozmika runs (raw=mach ticks + scale conversion once)",
    )
    parser.add_argument(
        "--out-name",
        default="single_op_window_crosslang.json",
        help="output json filename under bench/results/primitives",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()

    selected = [item.strip() for item in args.languages.split(",") if item.strip()]
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    results: list[WindowResult] = []

    if "kozmika-interpret" in selected:
        results.append(
            run_kozmika(
                primitive=args.primitive,
                operator=args.operator,
                loops=args.loops,
                runs=args.runs,
                mode="interpret",
                a_lit=args.a,
                b_lit=args.b,
                tick_mode=args.kozmika_tick_mode,
                batch=args.batch,
            )
        )
    if "kozmika-native" in selected:
        results.append(
            run_kozmika(
                primitive=args.primitive,
                operator=args.operator,
                loops=args.loops,
                runs=args.runs,
                mode="native",
                a_lit=args.a,
                b_lit=args.b,
                tick_mode=args.kozmika_tick_mode,
                batch=args.batch,
            )
        )
    if "c" in selected:
        row = run_c_like("c", args.operator, args.loops, args.batch, args.runs, args.a, args.b)
        if row:
            results.append(row)
    if "cpp" in selected:
        row = run_c_like("cpp", args.operator, args.loops, args.batch, args.runs, args.a, args.b)
        if row:
            results.append(row)
    if "csharp" in selected:
        row = run_csharp(args.operator, args.loops, args.batch, args.runs, args.a, args.b)
        if row:
            results.append(row)

    out_path = RESULT_DIR / args.out_name
    out_path.write_text(
        json.dumps(
            {
                "method": "window: t1=now(); repeat(batch){c=a op b}; t2=now(); floor=assign-only",
                "primitive": args.primitive,
                "operator": args.operator,
                "loops": args.loops,
                "batch": args.batch,
                "runs": args.runs,
                "results": [asdict(row) for row in results],
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    print("| language | mode | raw(ns) | floor(ns) | net(ns) | checksum |")
    print("|---|---|---:|---:|---:|---|")
    for row in results:
        print(
            f"| {row.language} | {row.mode} | {row.raw_ns:.3f} | {row.floor_ns:.3f} | {row.net_ns:.3f} | {row.checksum} |"
        )
    print(f"result_json: {out_path}")
    return 0
