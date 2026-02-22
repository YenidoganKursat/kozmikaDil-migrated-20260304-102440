"""Constants and primitive metadata for random operator benchmarks."""

from __future__ import annotations

INT_PRIMITIVES = ["i8", "i16", "i32", "i64", "i128", "i256", "i512"]
FLOAT_PRIMITIVES = ["f8", "f16", "bf16", "f32", "f64", "f128", "f256", "f512"]
PRIMITIVES = INT_PRIMITIVES + FLOAT_PRIMITIVES
WIDE_PRIMITIVES = {"i128", "i256", "i512", "f128", "f256", "f512"}
INT_BITS = {
    "i8": 8,
    "i16": 16,
    "i32": 32,
    "i64": 64,
    "i128": 128,
    "i256": 256,
    "i512": 512,
}
OPS: list[tuple[str, str]] = [
    ("add", "+"),
    ("sub", "-"),
    ("mul", "*"),
    ("div", "/"),
    ("mod", "%"),
    ("pow", "^"),
]


def is_float_primitive(primitive: str) -> bool:
    return primitive in FLOAT_PRIMITIVES


def is_wide_primitive(primitive: str) -> bool:
    return primitive in WIDE_PRIMITIVES

