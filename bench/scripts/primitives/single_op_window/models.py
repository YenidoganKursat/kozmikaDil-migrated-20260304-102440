"""Data models for single-op window benchmark results."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class WindowResult:
    language: str
    mode: str
    primitive: str
    operator: str
    loops: int
    batch: int
    runs: int
    floor_ns: float
    raw_ns: float
    net_ns: float
    checksum: str
    notes: str = ""
