"""Program generators for builtin/native/interpret benchmark modes."""

from __future__ import annotations

import pathlib
from typing import List


def make_builtin_program(path: pathlib.Path, primitive: str, operator: str, loops: int, seed_x: int, seed_y: int) -> None:
    source = "\n".join(
        [
            f"acc = bench_mixed_numeric_op(\"{primitive}\", \"{operator}\", {loops}, {seed_x}, {seed_y})",
            "print(acc)",
            "",
        ]
    )
    path.write_text(source, encoding="utf-8")


def make_operator_program(path: pathlib.Path, primitive: str, operator: str, loops: int) -> None:
    """Generate a benchmark program that uses the language operator directly."""
    lines: List[str] = [
        "seed_x = i64(123456789)",
        "seed_y = i64(362436069)",
        "i = 0",
        "acc = f64(0)",
        f"while i < {loops}:",
        "  seed_x = (seed_x * 1664525 + 1013904223) % 2147483648",
        "  seed_y = (seed_y * 22695477 + 1) % 2147483648",
    ]
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
    if operator in ("/", "%"):
        lines += [
            "  if y == 0:",
            f"    y = {primitive}(0.5)",
        ]
    lines += [
        f"  tmp = x {operator} y",
        "  acc = acc + f64(tmp)",
        "  i = i + 1",
        "print(acc)",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")

