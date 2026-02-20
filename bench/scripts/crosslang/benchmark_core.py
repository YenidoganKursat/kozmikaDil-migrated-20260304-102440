from __future__ import annotations

import os
import pathlib
import re
import statistics
import subprocess
import time
from dataclasses import dataclass
from typing import Dict, List, Tuple


@dataclass
class SampleBatch:
    times: List[float]
    checksum_raw: str
    checksum: float


@dataclass
class BenchmarkResult:
    name: str
    median_sec: float
    min_sec: float
    max_sec: float
    runs: int
    checksum_raw: str
    checksum: float
    vs_c: float | None = None
    mode: str = "full"
    compute_median_sec: float | None = None
    init_median_sec: float | None = None


def run_checked(cmd: List[str], *, cwd: pathlib.Path, env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(cmd, cwd=str(cwd), env=merged_env, capture_output=True, text=True, check=True)


def parse_checksum(stdout: str) -> Tuple[str, float]:
    float_line = None
    for raw in reversed(stdout.strip().splitlines()):
        line = raw.strip().replace(",", ".")
        if re.fullmatch(r"[+-]?\d+(\.\d+)?([eE][+-]?\d+)?", line):
            float_line = line
            break
    if float_line is None:
        raise RuntimeError(f"could not parse checksum from output:\n{stdout}")
    return float_line, float(float_line)


def measure_samples(cmd: List[str], cwd: pathlib.Path, env: Dict[str, str], warmup: int, runs: int) -> SampleBatch:
    for _ in range(warmup):
        run_checked(cmd, cwd=cwd, env=env)

    times: List[float] = []
    checksum_raw = ""
    checksum = 0.0
    for _ in range(runs):
        t0 = time.perf_counter()
        proc = run_checked(cmd, cwd=cwd, env=env)
        t1 = time.perf_counter()
        times.append(t1 - t0)
        checksum_raw, checksum = parse_checksum(proc.stdout)
    return SampleBatch(times=times, checksum_raw=checksum_raw, checksum=checksum)


def result_from_full(name: str, batch: SampleBatch, mode: str) -> BenchmarkResult:
    return BenchmarkResult(
        name=name,
        median_sec=statistics.median(batch.times),
        min_sec=min(batch.times),
        max_sec=max(batch.times),
        runs=len(batch.times),
        checksum_raw=batch.checksum_raw,
        checksum=batch.checksum,
        mode=mode,
    )


def result_from_kernel_delta(name: str, compute: SampleBatch, init: SampleBatch) -> BenchmarkResult:
    pair_count = min(len(compute.times), len(init.times))
    if pair_count <= 0:
        raise RuntimeError(f"kernel-only delta requires non-empty samples for {name}")
    delta_times = [max(0.0, compute.times[i] - init.times[i]) for i in range(pair_count)]
    return BenchmarkResult(
        name=name,
        median_sec=statistics.median(delta_times),
        min_sec=min(delta_times),
        max_sec=max(delta_times),
        runs=pair_count,
        checksum_raw=compute.checksum_raw,
        checksum=compute.checksum,
        mode="kernel-only",
        compute_median_sec=statistics.median(compute.times),
        init_median_sec=statistics.median(init.times),
    )
