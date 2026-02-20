"""Shared constants for mixed signed primitive runtime benchmarks."""

from __future__ import annotations

from typing import List, Tuple

PRIMITIVES = ["f8", "f16", "f32", "f64", "f128", "f256", "f512"]

OPS: List[Tuple[str, str]] = [
    ("add", "+"),
    ("sub", "-"),
    ("mul", "*"),
    ("div", "/"),
    ("mod", "%"),
    ("pow", "^"),
]

