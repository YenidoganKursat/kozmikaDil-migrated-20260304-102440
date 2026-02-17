import math
import statistics
import subprocess
import time
from typing import Dict, List, Tuple


def run_once(command: List[str], env: Dict[str, str]) -> Tuple[int, str, float]:
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        check=False,
    )
    end = time.perf_counter()
    return proc.returncode, proc.stdout, end - start


def collect_timing_samples(
    command: List[str],
    env: Dict[str, str],
    runs: int,
    warmup_runs: int,
    sample_repeat: int,
) -> Tuple[List[int], str, List[float]]:
    first_output = ""
    statuses: List[int] = []
    samples: List[float] = []

    for _ in range(max(0, warmup_runs)):
        for _ in range(max(1, sample_repeat)):
            run_once(command, env)

    for i in range(max(1, runs)):
        start = time.perf_counter()
        output = ""
        for rep in range(max(1, sample_repeat)):
            status, current_output, _ = run_once(command, env)
            statuses.append(status)
            if i == 0 and rep == 0:
                output = current_output
        elapsed = time.perf_counter() - start
        samples.append(elapsed)
        if i == 0:
            first_output = output
    return statuses, first_output, samples


def parse_phase7_output(output_text: str) -> Dict[str, float]:
    tokens = [line.strip() for line in output_text.strip().splitlines() if line.strip()]
    if len(tokens) < 9:
        raise ValueError(f"expected at least 9 numeric lines, got {len(tokens)}")

    values = []
    for token in tokens[:9]:
        try:
            values.append(float(token))
        except ValueError as exc:
            raise ValueError(f"non-numeric output token: {token}") from exc

    return {
        "checksum": values[0],
        "analyze_count": values[1],
        "materialize_count": values[2],
        "cache_hit_count": values[3],
        "fused_count": values[4],
        "fallback_count": values[5],
        "last_allocations": values[6],
        "total_allocations": values[7],
        "plan_id": values[8],
    }


def summarize_times(samples: List[float], drift_limit: float) -> Dict[str, float]:
    if not samples:
        return {
            "sample_count": 0,
            "median_time_sec": 0.0,
            "mean_time_sec": 0.0,
            "stdev_time_sec": 0.0,
            "max_drift_percent": 0.0,
            "reproducible": 0.0,
        }

    ordered = sorted(samples)
    trimmed = ordered[1:-1] if len(ordered) > 4 else ordered
    if not trimmed:
        trimmed = ordered

    median = statistics.median(trimmed)
    mean = statistics.fmean(trimmed)
    stdev = statistics.pstdev(trimmed) if len(trimmed) > 1 else 0.0
    if median > 0:
        drift = [abs((s - median) / median) * 100.0 for s in trimmed]
        max_drift = max(drift)
    else:
        max_drift = 0.0

    return {
        "sample_count": float(len(samples)),
        "median_time_sec": median,
        "mean_time_sec": mean,
        "stdev_time_sec": stdev,
        "max_drift_percent": max_drift,
        "reproducible": 1.0 if max_drift <= drift_limit else 0.0,
    }


def within_tolerance(actual: float, expected: float, rel_eps: float = 1e-9) -> bool:
    if math.isfinite(expected):
        return abs(actual - expected) <= rel_eps * max(1.0, abs(expected))
    return actual == expected
