"""Shared utilities for fair float cross-language benchmarking."""

from __future__ import annotations

import os
import pathlib
import statistics
import subprocess
from dataclasses import dataclass
from typing import Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"

OPS = ["+", "-", "*", "/", "%", "^"]
FLOAT_PRIMITIVES = ["f8", "f16", "f32", "f64", "f128", "f256", "f512"]
BITS = {
    "f8": 8,
    "f16": 16,
    "f32": 32,
    "f64": 64,
    "f128": 128,
    "f256": 256,
    "f512": 512,
}


@dataclass
class BenchRow:
    ns_op: float
    checksum: str


def run_checked(
    cmd: list[str],
    cwd: pathlib.Path | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=str(cwd or REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
        env=merged,
    )


def runtime_env() -> dict[str, str]:
    return {
        "OPENBLAS_NUM_THREADS": "1",
        "OMP_NUM_THREADS": "1",
        "MKL_NUM_THREADS": "1",
        "VECLIB_MAXIMUM_THREADS": "1",
        "BLIS_NUM_THREADS": "1",
    }


def median_ns_op(samples_sec: Iterable[float], loops: int) -> float:
    return statistics.median(samples_sec) * 1.0e9 / float(loops)
