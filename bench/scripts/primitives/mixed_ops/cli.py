"""CLI orchestration for mixed signed primitive operator benchmark."""

from __future__ import annotations

import argparse
import pathlib
from typing import Dict, List, Tuple

from .constants import OPS, PRIMITIVES
from .io_utils import write_result_files
from .runners import benchmark_builtin, benchmark_interpret, benchmark_native


def run_cli(repo_root: pathlib.Path) -> int:
    parser = argparse.ArgumentParser(description="Benchmark mixed-sign primitive ops runtime")
    parser.add_argument("--loops", type=int, default=100_000_000)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--seed-x", type=int, default=123456789)
    parser.add_argument("--seed-y", type=int, default=362436069)
    parser.add_argument("--primitives", type=str, default="")
    parser.add_argument("--ops", type=str, default="")
    parser.add_argument("--backend", choices=["auto", "builtin", "native", "interpret"], default="auto")
    args = parser.parse_args()

    out_dir = repo_root / "bench" / "results" / "primitives"
    out_dir.mkdir(parents=True, exist_ok=True)

    selected_primitives = _parse_primitives(args.primitives)
    selected_ops = _parse_ops(args.ops)

    rows: List[Dict[str, object]] = []
    for primitive in selected_primitives:
        for op_name, operator in selected_ops:
            backend = "builtin" if args.backend == "auto" else args.backend
            if backend == "native":
                row = benchmark_native(
                    repo_root=repo_root,
                    primitive=primitive,
                    op_name=op_name,
                    operator=operator,
                    loops=args.loops,
                    warmup=args.warmup,
                    runs=args.runs,
                )
            elif backend == "interpret":
                row = benchmark_interpret(
                    repo_root=repo_root,
                    primitive=primitive,
                    op_name=op_name,
                    operator=operator,
                    loops=args.loops,
                    warmup=args.warmup,
                    runs=args.runs,
                )
            else:
                row = benchmark_builtin(
                    repo_root=repo_root,
                    primitive=primitive,
                    op_name=op_name,
                    operator=operator,
                    loops=args.loops,
                    warmup=args.warmup,
                    runs=args.runs,
                    seed_x=args.seed_x,
                    seed_y=args.seed_y,
                )

            rows.append(row)
            print(
                f"{primitive:<5} {operator:<1} backend={row['backend']:<9} "
                f"median={row['median_sec']:.6f}s min={row['min_sec']:.6f}s "
                f"max={row['max_sec']:.6f}s checksum={row['checksum']}"
            )

    json_path, csv_path = write_result_files(
        out_dir=out_dir,
        loops=args.loops,
        runs=args.runs,
        warmup=args.warmup,
        seed_x=args.seed_x,
        seed_y=args.seed_y,
        backend=args.backend,
        rows=rows,
    )
    print(f"result_json: {json_path}")
    print(f"result_csv: {csv_path}")
    return 0


def _parse_primitives(raw: str) -> List[str]:
    if not raw:
        return PRIMITIVES
    selected = [item.strip() for item in raw.split(",") if item.strip()]
    unknown = [item for item in selected if item not in set(PRIMITIVES)]
    if unknown:
        raise SystemExit(f"unknown primitives: {','.join(unknown)}")
    return selected


def _parse_ops(raw: str) -> List[Tuple[str, str]]:
    if not raw:
        return OPS
    names = [item.strip() for item in raw.split(",") if item.strip()]
    allowed = {name for name, _ in OPS}
    unknown = [item for item in names if item not in allowed]
    if unknown:
        raise SystemExit(f"unknown ops: {','.join(unknown)}")
    lookup = {name: sym for name, sym in OPS}
    return [(name, lookup[name]) for name in names]

