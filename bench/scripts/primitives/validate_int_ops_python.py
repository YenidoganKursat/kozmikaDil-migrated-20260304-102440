#!/usr/bin/env python3
"""Validate integer primitive kernels against Python reference semantics.

Coverage:
- random stream checks (legacy behavior),
- deterministic extreme vectors (boundaries + random edge fuzz),
- all integer primitives: i8..i512,
- all operators: +,-,*,/,%,^.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import random
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


@dataclass
class ExtremeValidationSummary:
    primitive: str
    operator: str
    cases: int
    max_abs_error: float
    mismatch_count: int
    pass_check: bool


def run_checked(cmd: List[str]) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            cmd,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        raise RuntimeError(
            "command failed: "
            + " ".join(cmd)
            + f"\nstdout:\n{stdout}\nstderr:\n{stderr}"
        ) from exc


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("empty output")
    return lines[-1]


def parse_lines(stdout: str) -> List[str]:
    return [line.strip() for line in stdout.splitlines() if line.strip()]


def effective_bits(primitive: str) -> int:
    return min(BITS[primitive], 128)


def bounds_for_bits(bits: int) -> Tuple[int, int]:
    if bits >= 128:
        lo = -(1 << 127)
        hi = (1 << 127) - 1
    else:
        lo = -(1 << (bits - 1))
        hi = (1 << (bits - 1)) - 1
    return lo, hi


def derive_seed(seed_base: int, material: str) -> int:
    acc = (seed_base & 0xFFFFFFFF) ^ 0x85EB_CA6B
    for idx, ch in enumerate(material):
        acc ^= (ord(ch) << ((idx % 4) * 8))
        acc = (acc * 1664525 + 1013904223) & 0x7FFFFFFF
    return acc


def clamp_signed(value: int, bits: int) -> int:
    lo, hi = bounds_for_bits(bits)
    if value < lo:
        return lo
    if value > hi:
        return hi
    return value


def signed_div_overflow(lhs: int, rhs: int, bits: int) -> bool:
    lo, _ = bounds_for_bits(bits)
    return rhs == -1 and lhs == lo


def c_style_mod(lhs: int, rhs: int) -> int:
    if rhs == 0:
        raise ZeroDivisionError("modulo by zero")
    q = math.trunc(lhs / rhs)
    return lhs - q * rhs


def python_reference(
    primitive: str,
    operator: str,
    loops: int,
    seed_x_init: int,
    seed_y_init: int,
) -> str:
    bits = effective_bits(primitive)
    seed_x = seed_x_init
    seed_y = seed_y_init
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
        if operator in ("/", "%") and signed_div_overflow(x, y, bits):
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


def python_reference_case(primitive: str, operator: str, x: int, y: int) -> str:
    bits = effective_bits(primitive)
    x = clamp_signed(x, bits)
    y = clamp_signed(y, bits)

    if operator in ("/", "%") and y == 0:
        y = 1
    if operator in ("/", "%") and signed_div_overflow(x, y, bits):
        y = 1
    if operator == "^" and x == 0 and y < 0:
        y = 1

    if operator == "+":
        return str(clamp_signed(x + y, bits))
    if operator == "-":
        return str(clamp_signed(x - y, bits))
    if operator == "*":
        return str(clamp_signed(x * y, bits))
    if operator == "/":
        return format(float(x) / float(y), ".15g")
    if operator == "%":
        return str(clamp_signed(c_style_mod(x, y), bits))
    if operator == "^":
        return format(math.pow(float(x), float(y)), ".15g")
    raise ValueError(f"unsupported operator: {operator}")


def make_program(
    path: pathlib.Path,
    primitive: str,
    operator: str,
    loops: int,
    seed_x_init: int,
    seed_y_init: int,
) -> None:
    bits = effective_bits(primitive)
    lo, _ = bounds_for_bits(bits)
    acc_decl = "f64(0)" if operator in ("/", "^") else f"{primitive}(0)"
    lines = [
        f"seed_x = i64({seed_x_init})",
        f"seed_y = i64({seed_y_init})",
        f"acc = {acc_decl}",
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
            f"  if x == {primitive}({lo}) and y == {primitive}(-1):",
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


def run_kozmika(
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    seed_x_init: int,
    seed_y_init: int,
) -> str:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-int-val-{primitive}-{op_name}-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}.k"
        make_program(program, primitive, operator, loops, seed_x_init, seed_y_init)
        proc = run_checked(["./k", "run", str(program)])
        return parse_last_line(proc.stdout)


def generate_extreme_cases(
    primitive: str,
    operator: str,
    random_cases: int,
    seed_base: int,
) -> List[Tuple[int, int]]:
    bits = effective_bits(primitive)
    lo, hi = bounds_for_bits(bits)
    parser_safe = min(2_147_483_647, hi, -lo)
    lo_eff = -parser_safe
    hi_eff = parser_safe
    near_hi = hi_eff - 1
    near_lo = lo_eff + 1

    pairs: List[Tuple[int, int]] = []
    if operator == "^":
        # Keep pow edge vectors in a numerically stable finite range.
        base_values = [-1024, -255, -32, -8, -2, -1, 0, 1, 2, 8, 32, 255, 1024]
        exp_values = [-8, -4, -3, -2, -1, 0, 1, 2, 3, 4, 8]
        for x in base_values:
            for y in exp_values:
                if x == 0 and y < 0:
                    continue
                pairs.append((x, y))
    elif operator in ("/", "%"):
        num_values = [lo_eff, near_lo, -4096, -1024, -255, -16, -1, 0, 1, 16, 255, 1024, 4096, near_hi, hi_eff]
        den_values = [1, 2, 3, 5, 7, 16, 31, 97, 255, 1024]
        for x in num_values:
            for y in den_values:
                pairs.append((x, y))
                if not signed_div_overflow(x, -y, bits):
                    pairs.append((x, -y))
    else:
        core_values = [lo_eff, near_lo, -4096, -1024, -255, -16, -2, -1, 0, 1, 2, 16, 255, 1024, 4096, near_hi, hi_eff]
        for x in core_values:
            for y in core_values:
                pairs.append((x, y))

    rng = random.Random(derive_seed(seed_base, f"extreme:{primitive}:{operator}:{bits}"))
    for _ in range(max(0, random_cases)):
        x = rng.randint(lo_eff, hi_eff)
        y = rng.randint(lo_eff, hi_eff)
        if operator in ("/", "%") and y == 0:
            y = 1
        if operator in ("/", "%") and signed_div_overflow(x, y, bits):
            y = 1
        if operator == "^":
            x = rng.randint(-32, 32)
            y = rng.randint(-8, 8)
            if x == 0 and y < 0:
                y = 1
        pairs.append((x, y))
    return pairs


def make_extreme_program(path: pathlib.Path, primitive: str, operator: str, cases: List[Tuple[int, int]]) -> None:
    bits = effective_bits(primitive)
    lo, _ = bounds_for_bits(bits)
    lines: List[str] = []
    for i, (x, y) in enumerate(cases):
        lines.append(f"x{i} = {primitive}({x})")
        lines.append(f"y{i} = {primitive}({y})")
        if operator in ("/", "%"):
            lines.append(f"if y{i} == 0:")
            lines.append(f"  y{i} = {primitive}(1)")
            lines.append(f"if x{i} == {primitive}({lo}) and y{i} == {primitive}(-1):")
            lines.append(f"  y{i} = {primitive}(1)")
        if operator == "^":
            lines.append(f"if x{i} == 0 and y{i} < 0:")
            lines.append(f"  y{i} = {primitive}(1)")
        lines.append(f"r{i} = x{i} {operator} y{i}")
        lines.append(f"print(r{i})")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def run_kozmika_extreme_batch(primitive: str, op_name: str, operator: str, cases: List[Tuple[int, int]]) -> List[str]:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-int-extreme-{primitive}-{op_name}-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}_extreme.k"
        make_extreme_program(program, primitive, operator, cases)
        proc = run_checked(["./k", "run", str(program)])
        return parse_lines(proc.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate integer primitive ops vs Python reference")
    parser.add_argument("--loops", type=int, default=200000)
    parser.add_argument(
        "--include-extremes",
        action="store_true",
        help="Run deterministic extreme-case validation in addition to random stream checks.",
    )
    parser.add_argument(
        "--extreme-random-cases",
        type=int,
        default=128,
        help="Extra deterministic random vectors per primitive/operator for extreme checks.",
    )
    parser.add_argument(
        "--fail-on-mismatch",
        action="store_true",
        help="Return non-zero exit code when any primitive/operator check fails.",
    )
    parser.add_argument("--random-seed", type=int, default=None)
    parser.add_argument("--randomize-seed", action="store_true")
    args = parser.parse_args()

    seed_base = args.random_seed
    if seed_base is None:
        seed_base = random.SystemRandom().randrange(1, 2**31) if args.randomize_seed else 0
    print(f"random_seed_base={seed_base}")

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    rows: List[ValidationRow] = []
    extreme_summaries: List[ExtremeValidationSummary] = []
    extreme_failures: List[Dict[str, object]] = []

    for primitive in INT_PRIMITIVES:
        for op_name, operator in OPS:
            seed_x = derive_seed(seed_base, f"stream-x:{primitive}:{operator}") % 2147483648
            seed_y = derive_seed(seed_base, f"stream-y:{primitive}:{operator}") % 2147483648
            if seed_x == 0:
                seed_x = 123456789
            if seed_y == 0:
                seed_y = 362436069
            k_out = run_kozmika(primitive, op_name, operator, args.loops, seed_x, seed_y)
            py_out = python_reference(primitive, operator, args.loops, seed_x, seed_y)
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

            if args.include_extremes:
                cases = generate_extreme_cases(primitive, operator, args.extreme_random_cases, seed_base)
                got = run_kozmika_extreme_batch(primitive, op_name, operator, cases)
                if len(got) != len(cases):
                    raise RuntimeError(
                        f"unexpected extreme output length for {primitive} {operator}: "
                        f"got={len(got)} expected={len(cases)}"
                    )
                max_abs = 0.0
                mismatches = 0
                for idx, ((x, y), out_raw) in enumerate(zip(cases, got)):
                    ref_raw = python_reference_case(primitive, operator, x, y)
                    try:
                        abs_err_case = abs(float(out_raw) - float(ref_raw))
                    except ValueError:
                        abs_err_case = float("inf")
                    ref_mag = max(1.0, abs(float(ref_raw))) if math.isfinite(float(ref_raw)) else 1.0
                    allowed = 1e-12
                    if operator in ("/", "^"):
                        allowed = max(1e-9, 1e-9 * ref_mag)
                    if abs_err_case > max_abs:
                        max_abs = abs_err_case
                    if abs_err_case > allowed:
                        mismatches += 1
                        if len(extreme_failures) < 64:
                            extreme_failures.append(
                                {
                                    "primitive": primitive,
                                    "operator": operator,
                                    "case_index": idx,
                                    "x": x,
                                    "y": y,
                                    "kozmika_output": out_raw,
                                    "python_reference": ref_raw,
                                    "abs_error": abs_err_case,
                                    "allowed": allowed,
                                }
                            )

                pass_extreme = mismatches == 0
                extreme_summaries.append(
                    ExtremeValidationSummary(
                        primitive=primitive,
                        operator=operator,
                        cases=len(cases),
                        max_abs_error=max_abs,
                        mismatch_count=mismatches,
                        pass_check=pass_extreme,
                    )
                )
                print(
                    f"{primitive:<5} {operator} extremes "
                    f"cases={len(cases)} max_abs={max_abs:.6e} mismatches={mismatches} pass={pass_extreme}"
                )

    out = {
        "loops": args.loops,
        "random_seed_base": seed_base,
        "randomize_seed": bool(args.randomize_seed),
        "rows": [asdict(row) for row in rows],
        "extreme_mode": args.include_extremes,
        "extreme_random_cases": args.extreme_random_cases,
        "extreme_rows": [asdict(row) for row in extreme_summaries],
        "extreme_failures_preview": extreme_failures,
    }
    out_json = RESULT_DIR / "int_ops_python_validation.json"
    out_json.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"result_json: {out_json}")

    if args.fail_on_mismatch:
        failed_random = [row for row in rows if not row.pass_check]
        failed_extreme = [row for row in extreme_summaries if not row.pass_check]
        if failed_random:
            print(f"validation_failed: random mismatches={len(failed_random)}")
            return 1
        if failed_extreme:
            print(f"validation_failed: extreme groups with mismatch={len(failed_extreme)}")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
