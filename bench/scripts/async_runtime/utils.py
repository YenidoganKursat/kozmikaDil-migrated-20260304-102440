import statistics
import subprocess
import time
from typing import Dict, List, Optional, Tuple


def run_once(command: List[str], env: Optional[Dict[str, str]] = None) -> Tuple[int, str, float]:
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        check=False,
    )
    elapsed = time.perf_counter() - start
    return proc.returncode, proc.stdout, elapsed


def collect_timing_samples(
    command: List[str],
    env: Optional[Dict[str, str]],
    runs: int,
    warmup_runs: int,
    sample_repeat: int,
) -> Tuple[List[int], str, List[float]]:
    for _ in range(max(0, warmup_runs)):
        for _ in range(max(1, sample_repeat)):
            run_once(command, env)

    statuses: List[int] = []
    samples: List[float] = []
    first_output = ""
    for i in range(max(1, runs)):
        start = time.perf_counter()
        run_output = ""
        for rep in range(max(1, sample_repeat)):
            status, out, _ = run_once(command, env)
            statuses.append(status)
            if i == 0 and rep == 0:
                run_output = out
        elapsed = time.perf_counter() - start
        samples.append(elapsed)
        if i == 0:
            first_output = run_output
    return statuses, first_output, samples


def summarize_times(samples: List[float], drift_limit: float) -> Dict[str, float]:
    if not samples:
        return {
            "sample_count": 0.0,
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
    drift = 0.0
    if median > 0.0:
        drift = max(abs((value - median) / median) * 100.0 for value in trimmed)
    return {
        "sample_count": float(len(samples)),
        "median_time_sec": median,
        "mean_time_sec": mean,
        "stdev_time_sec": stdev,
        "max_drift_percent": drift,
        "reproducible": 1.0 if drift <= drift_limit else 0.0,
    }


def parse_numeric_lines(output: str, expected_min_lines: int) -> List[float]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(lines) < expected_min_lines:
        raise ValueError(f"expected >= {expected_min_lines} output lines, got {len(lines)}")
    values: List[float] = []
    for token in lines[:expected_min_lines]:
        values.append(float(token))
    return values
