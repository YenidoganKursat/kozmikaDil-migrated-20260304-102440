"""CLI for fair float cross-language runtime benchmark."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict

from .common import FLOAT_PRIMITIVES, OPS, RESULT_DIR
from .runners import run_c_like, run_csharp, run_go, run_java, run_kozmika


def _rows_to_dict(rows: dict[str, dict[str, object]]) -> dict[str, dict[str, dict[str, object]]]:
    out: dict[str, dict[str, dict[str, object]]] = {}
    for key, op_map in rows.items():
        out[key] = {}
        for op, row in op_map.items():
            out[key][op] = asdict(row)
    return out


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fair float runtime benchmark across languages")
    parser.add_argument("--loops", type=int, default=200_000)
    parser.add_argument("--lanes", type=int, default=1, help="independent ops per loop iteration")
    parser.add_argument("--pow-profile", choices=["generic", "hot"], default="generic")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--skip-cross-lang", action="store_true")
    parser.add_argument(
        "--kozmika-modes",
        default="interpret,native",
        help="comma-separated: interpret,native",
    )
    parser.add_argument(
        "--interpret-path",
        choices=["kernel", "ast", "hybrid"],
        default="kernel",
        help="interpret mode path selection for risky ops",
    )
    parser.add_argument("--out-name", default="float_crosslang_fair_runtime.json")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    RESULT_DIR.mkdir(parents=True, exist_ok=True)

    modes = [item.strip() for item in args.kozmika_modes.split(",") if item.strip()]
    if not modes:
        modes = ["interpret"]

    kozmika_by_mode: dict[str, dict[str, dict[str, object]]] = {}
    for mode in modes:
        mode_map: dict[str, dict[str, object]] = {}
        for primitive in FLOAT_PRIMITIVES:
            op_rows: dict[str, object] = {}
            for op in OPS:
                row = run_kozmika(
                    primitive=primitive,
                    operator=op,
                    loops=args.loops,
                    lanes=args.lanes,
                    pow_profile=args.pow_profile,
                    runs=args.runs,
                    warmup=args.warmup,
                    mode=mode,
                    interpret_path=args.interpret_path,
                )
                op_rows[op] = row
                print(
                    f"kozmika/{mode:<9} {primitive:<4} {op} "
                    f"{row.ns_op:10.3f} ns/op checksum={row.checksum}",
                    flush=True,
                )
            mode_map[primitive] = op_rows
        kozmika_by_mode[mode] = _rows_to_dict(mode_map)

    cross: dict[str, dict[str, dict[str, object]]] = {}
    if not args.skip_cross_lang:
        cross_runners = [
            (
                "c",
                lambda op: run_c_like("c", op, args.loops, args.lanes, args.pow_profile, args.runs, args.warmup),
            ),
            (
                "cpp",
                lambda op: run_c_like("cpp", op, args.loops, args.lanes, args.pow_profile, args.runs, args.warmup),
            ),
            ("go", lambda op: run_go(op, args.loops, args.lanes, args.pow_profile, args.runs, args.warmup)),
            (
                "csharp",
                lambda op: run_csharp(op, args.loops, args.lanes, args.pow_profile, args.runs, args.warmup),
            ),
            ("java", lambda op: run_java(op, args.loops, args.lanes, args.pow_profile, args.runs, args.warmup)),
        ]
        for language, runner in cross_runners:
            op_rows: dict[str, object] = {}
            available = True
            for op in OPS:
                row = runner(op)
                if row is None:
                    available = False
                    break
                op_rows[op] = row
                print(
                    f"{language:<7} f64  {op} {row.ns_op:10.3f} ns/op checksum={row.checksum}",
                    flush=True,
                )
            if available:
                cross[language] = _rows_to_dict({"f64": op_rows})["f64"]
            else:
                print(f"{language:<7} unavailable on this machine (skipped)", flush=True)

    payload = {
        "method": "fair total runtime over full loop, deterministic float stream, median ns/op",
        "loops": args.loops,
        "lanes": args.lanes,
        "pow_profile": args.pow_profile,
        "runs": args.runs,
        "warmup": args.warmup,
        "interpret_path": args.interpret_path,
        "ops": OPS,
        "kozmika_f_series": kozmika_by_mode,
        "cross_lang_f64": cross,
    }
    out_path = RESULT_DIR / args.out_name
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"result_json: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
