"""Program generation for random primitive operator benchmark cases."""

from __future__ import annotations

import math
import pathlib

from .constants import INT_BITS, is_float_primitive


def hash_int_step_expr(_primitive: str) -> list[str]:
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


def make_program(path: pathlib.Path, primitive: str, operator: str, loops: int, checksum_mode: str) -> None:
    # Deterministic pseudo-random streams:
    # x_n = LCG(seed_x), y_n = LCG(seed_y)
    # Each iteration computes exactly one operator kernel: acc = x <op> y
    # and prints final accumulator after loops.
    lines: list[str] = [
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

