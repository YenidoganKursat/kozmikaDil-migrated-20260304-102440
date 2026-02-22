"""Data models used by random operator benchmark flow."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class OpRuntime:
    primitive: str
    op_name: str
    operator: str
    profile: str
    exec_mode: str
    loops: int
    runs: int
    warmup: int
    median_sec: float
    min_sec: float
    max_sec: float
    checksum_raw: str

