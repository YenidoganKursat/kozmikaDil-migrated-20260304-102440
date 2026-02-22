"""Checksum/tolerance policies for random operator benchmarks."""

from __future__ import annotations

from decimal import Decimal, InvalidOperation

from .constants import INT_PRIMITIVES


def parse_decimal(text: str) -> Decimal | None:
    try:
        return Decimal(text.strip())
    except (InvalidOperation, AttributeError):
        return None


def tolerance_for_primitive(primitive: str, _safety_tier: str) -> Decimal:
    if primitive in INT_PRIMITIVES:
        return Decimal("0")
    if primitive == "f8":
        return Decimal("1e-3")
    if primitive in ("f16", "bf16"):
        return Decimal("1e-6")
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
) -> tuple[bool, str, str, str]:
    b = parse_decimal(baseline_checksum)
    o = parse_decimal(optimized_checksum)
    if b is None or o is None:
        tol = str(tolerance_for_primitive(primitive, safety_tier))
        return (baseline_checksum == optimized_checksum, "", "", tol)
    if not b.is_finite() or not o.is_finite():
        tol = str(tolerance_for_primitive(primitive, safety_tier))
        return (str(b) == str(o), "", "", tol)
    try:
        abs_diff = abs(b - o)
        denom = max(abs(b), abs(o), Decimal("1"))
        rel_diff = abs_diff / denom
    except InvalidOperation:
        tol = str(tolerance_for_primitive(primitive, safety_tier))
        return (baseline_checksum == optimized_checksum, "", "", tol)
    tol_val = tolerance_for_primitive(primitive, safety_tier)
    ok = abs_diff <= tol_val or rel_diff <= tol_val
    return (ok, format(abs_diff, "e"), format(rel_diff, "e"), str(tol_val))

