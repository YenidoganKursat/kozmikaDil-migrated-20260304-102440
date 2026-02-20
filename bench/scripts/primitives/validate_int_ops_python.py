#!/usr/bin/env python3
"""Validate integer primitive random-op kernels against Python reference model.

Reference model mirrors current Kozmika integer semantics:
- signed saturating arithmetic with effective width min(bits, 128),
- C-style integer modulo (remainder sign follows lhs),
- integer division yields floating-point value.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from typing import Dict, List, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"

INT_PRIMITIVES = ["i8", "i16", "i32", "i64", "i128", "i256", "i512"]
OPS: List[Tuple[str, str]] = [
    ("add", "+"),
    ("sub", "-"),
    ("mul", "*"),
    ("div", "/"),
    ("mod", "%"),
    ("pow", "^"),
]

BITS: Dict[str, int] = {
    "i8": 8,
    "i16": 16,
    "i32": 32,
    "i64": 64,
    "i128": 128,
    "i256": 256,
    "i512": 512,
}


@dataclass
class ValidationRow:
    primitive: str
    operator: str
    loops: int
    kozmika_output: str
    python_reference: str
    abs_error: float
    pass_check: bool


def run_checked(cmd: List[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
    )


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("empty output")
    return lines[-1]


def effective_bits(primitive: str) -> int:
    return min(BITS[primitive], 128)


def clamp_signed(value: int, bits: int) -> int:
    if bits >= 128:
        lo = -(1 << 127)
        hi = (1 << 127) - 1
    else:
        lo = -(1 << (bits - 1))
        hi = (1 << (bits - 1)) - 1
    if value < lo:
        return lo
    if value > hi:
        return hi
    return value


def c_style_mod(lhs: int, rhs: int) -> int:
    if rhs == 0:
        raise ZeroDivisionError("modulo by zero")
    q = math.trunc(lhs / rhs)
    return lhs - q * rhs


def python_reference(primitive: str, operator: str, loops: int) -> str:
    bits = effective_bits(primitive)
    seed_x = 123456789
    seed_y = 362436069
    acc_int = 0
    acc_float = 0.0
    is_float = operator == "/"

    for _ in range(loops):
        seed_x = (seed_x * 1664525 + 1013904223) % 2147483648
        seed_y = (seed_y * 22695477 + 1) % 2147483648
        x = clamp_signed(seed_x - 1073741824, bits)
        y = clamp_signed(seed_y - 1073741824, bits)
        if operator in ("/", "%") and y == 0:
            y = 1
        if operator == "^":
            # Keep integer-power validation in a stable finite domain.
            x = clamp_signed((seed_x % 33) - 16, bits)
            y = (seed_y % 9) - 4
            if x == 0 and y < 0:
                y = 1

        if operator == "+":
            acc_int = clamp_signed(x + y, bits)
            is_float = False
        elif operator == "-":
            acc_int = clamp_signed(x - y, bits)
            is_float = False
        elif operator == "*":
            acc_int = clamp_signed(x * y, bits)
            is_float = False
        elif operator == "/":
            acc_float = float(x) / float(y)
            is_float = True
        elif operator == "%":
            acc_int = c_style_mod(x, y)
            acc_int = clamp_signed(acc_int, bits)
            is_float = False
        elif operator == "^":
            acc_float = math.pow(float(x), float(y))
            is_float = True
        else:
            raise ValueError(f"unsupported operator: {operator}")

    if is_float:
        return format(acc_float, ".15g")
    return str(acc_int)


def make_program(path: pathlib.Path, primitive: str, operator: str, loops: int) -> None:
    lines = [
        "seed_x = i64(123456789)",
        "seed_y = i64(362436069)",
        f"acc = {primitive}(0)",
        "i = 0",
        f"while i < {loops}:",
        "  seed_x = (seed_x * 1664525 + 1013904223) % 2147483648",
        "  seed_y = (seed_y * 22695477 + 1) % 2147483648",
        f"  x = {primitive}(seed_x - 1073741824)",
        f"  y = {primitive}(seed_y - 1073741824)",
    ]
    if operator in ("/", "%"):
        lines += [
            "  if y == 0:",
            f"    y = {primitive}(1)",
        ]
    if operator == "^":
        lines += [
            f"  x = {primitive}((seed_x % 33) - 16)",
            f"  y = {primitive}((seed_y % 9) - 4)",
            "  if x == 0 and y < 0:",
            f"    y = {primitive}(1)",
        ]
    lines += [
        f"  acc = x {operator} y",
        "  i = i + 1",
        "print(acc)",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def run_kozmika(primitive: str, op_name: str, operator: str, loops: int) -> str:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-int-val-{primitive}-{op_name}-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}.k"
        make_program(program, primitive, operator, loops)
        proc = run_checked(["./k", "run", "--interpret", str(program)])
        return parse_last_line(proc.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate integer primitive ops vs Python reference")
    parser.add_argument("--loops", type=int, default=200000)
    parser.add_argument(
        "--fail-on-mismatch",
        action="store_true",
        help="Return non-zero exit code when any primitive/operator check fails.",
    )
    args = parser.parse_args()

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    rows: List[ValidationRow] = []
    for primitive in INT_PRIMITIVES:
        for op_name, operator in OPS:
            k_out = run_kozmika(primitive, op_name, operator, args.loops)
            py_out = python_reference(primitive, operator, args.loops)
            try:
                abs_err = abs(float(k_out) - float(py_out))
            except ValueError:
                abs_err = float("inf")
            pass_check = abs_err <= 1e-12
            rows.append(
                ValidationRow(
                    primitive=primitive,
                    operator=operator,
                    loops=args.loops,
                    kozmika_output=k_out,
                    python_reference=py_out,
                    abs_error=abs_err,
                    pass_check=pass_check,
                )
            )
            print(
                f"{primitive:<5} {operator} "
                f"kozmika={k_out} python={py_out} abs_err={abs_err:.6e} pass={pass_check}"
            )

    out = {
        "loops": args.loops,
        "rows": [asdict(row) for row in rows],
    }
    out_json = RESULT_DIR / "int_ops_python_validation.json"
    out_json.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"result_json: {out_json}")
    if args.fail_on_mismatch:
        failed = [row for row in rows if not row.pass_check]
        if failed:
            print(f"validation_failed: {len(failed)} mismatches")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
