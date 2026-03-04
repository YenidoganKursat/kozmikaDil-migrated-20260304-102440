"""Shared utilities for fair integer cross-language benchmarking."""

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
INT_PRIMITIVES = ["i8", "i16", "i32", "i64", "i128", "i256", "i512"]
BITS = {
    "i8": 8,
    "i16": 16,
    "i32": 32,
    "i64": 64,
    "i128": 128,
    "i256": 256,
    "i512": 512,
}


@dataclass
class BenchRow:
    ns_op: float
    checksum: str


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("benchmark output is empty")
    return lines[-1]


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
    # Keep one-core runtime behavior stable across language runners.
    return {
        "OPENBLAS_NUM_THREADS": "1",
        "OMP_NUM_THREADS": "1",
        "MKL_NUM_THREADS": "1",
        "VECLIB_MAXIMUM_THREADS": "1",
        "BLIS_NUM_THREADS": "1",
    }


def median_ns_op(samples_sec: Iterable[float], loops: int) -> float:
    return statistics.median(samples_sec) * 1.0e9 / float(loops)

