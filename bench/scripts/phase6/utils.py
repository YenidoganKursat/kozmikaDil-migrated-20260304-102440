import math
import os
import shutil
import statistics
import subprocess
import time
from typing import Dict, List


def run_once(command: List[str], capture_output: bool = True):
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE if capture_output else subprocess.DEVNULL,
        stderr=subprocess.PIPE if capture_output else subprocess.DEVNULL,
        check=False,
    )
    end = time.perf_counter()
    stdout = proc.stdout if proc.stdout is not None else ""
    stderr = proc.stderr if proc.stderr is not None else ""
    return proc.returncode, stdout, stderr, end - start


def _run_repeated_sample(command: List[str], sample_repeat: int, capture_first_stdout: bool):
    start = time.perf_counter()
    status_codes = []
    first_stdout = ""
    for rep in range(max(1, sample_repeat)):
        capture_output = capture_first_stdout and rep == 0
        status, stdout, _, _ = run_once(command, capture_output=capture_output)
        status_codes.append(status)
        if rep == 0 and capture_output:
            first_stdout = stdout
    end = time.perf_counter()
    return status_codes, first_stdout, end - start


def collect_timing_samples(command: List[str], runs: int, warmup_runs: int, sample_repeat: int):
    for _ in range(max(0, warmup_runs)):
        _run_repeated_sample(command, sample_repeat=sample_repeat, capture_first_stdout=False)

    statuses = []
    outputs = []
    times = []
    for sample_index in range(max(1, runs)):
        sample_statuses, stdout, elapsed = _run_repeated_sample(
            command,
            sample_repeat=sample_repeat,
            capture_first_stdout=(sample_index == 0),
        )
        statuses.extend(sample_statuses)
        times.append(elapsed)
        if sample_index == 0:
            outputs.append(stdout)
    return statuses, (outputs[0] if outputs else ""), times


def summarize_times(samples: List[float], drift_limit_percent: float) -> Dict[str, float]:
    ordered = sorted(samples)
    trimmed = ordered[1:-1] if len(ordered) > 4 else ordered
    if not trimmed:
        trimmed = ordered

    median_time = statistics.median(trimmed) if trimmed else 0.0
    mean_time = statistics.fmean(trimmed) if trimmed else 0.0
    stdev_time = statistics.pstdev(trimmed) if len(trimmed) > 1 else 0.0
    min_time = min(trimmed) if trimmed else 0.0
    max_time = max(trimmed) if trimmed else 0.0
    if median_time > 0.0:
        drifts = [abs((value - median_time) / median_time) * 100.0 for value in trimmed]
        max_drift = max(drifts) if drifts else 0.0
    else:
        max_drift = 0.0

    return {
        "sample_count": len(samples),
        "trimmed_count": len(trimmed),
        "mean_time_sec": mean_time,
        "median_time_sec": median_time,
        "stdev_time_sec": stdev_time,
        "min_time_sec": min_time,
        "max_time_sec": max_time,
        "max_drift_percent": max_drift,
        "reproducible": bool(max_drift <= drift_limit_percent),
    }


def parse_phase6_output(output: str) -> Dict[str, float]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(lines) < 8:
        raise ValueError(f"phase6 benchmark output expected >=8 lines, got {len(lines)}")

    def as_float(index: int) -> float:
        token = lines[index]
        if token.lower() == "true":
            return 1.0
        if token.lower() == "false":
            return 0.0
        return float(token)

    def as_int(index: int) -> int:
        return int(round(as_float(index)))

    return {
        "total": as_float(0),
        "plan_used": as_int(1),
        "analyze_count": as_int(2),
        "materialize_count": as_int(3),
        "cache_hit_count": as_int(4),
        "invalidation_count": as_int(5),
        "cache_bytes": as_int(6),
        "inspect_plan": as_int(7),
    }


def within_tolerance(actual: float, expected: float, eps: float = 1e-9) -> bool:
    scale = max(1.0, abs(expected))
    return abs(actual - expected) <= eps * scale


def try_collect_perf(command: List[str], ops_per_run: int) -> Dict[str, object]:
    if os.uname().sysname != "Linux":
        return {"available": False, "reason": "perf not supported on this OS"}
    perf_bin = shutil.which("perf")
    if not perf_bin:
        return {"available": False, "reason": "perf not found"}

    perf_cmd = [
        perf_bin,
        "stat",
        "-x,",
        "-e",
        "cycles,instructions,branch-misses,cache-misses",
        "--",
        *command,
    ]
    status, _, stderr, _ = run_once(perf_cmd, capture_output=True)
    if status != 0:
        return {"available": False, "reason": "perf stat failed"}

    metrics = {}
    for raw in stderr.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 3:
            continue
        value_token = parts[0]
        event_name = parts[2]
        if value_token in {"<not counted>", "<not supported>", ""}:
            continue
        cleaned = value_token.replace(" ", "")
        cleaned = cleaned.replace(",", "")
        try:
            metrics[event_name] = float(cleaned)
        except ValueError:
            continue

    if not metrics:
        return {"available": False, "reason": "perf stat had no numeric counters"}

    per_op = {}
    divisor = max(1, ops_per_run)
    for key, value in metrics.items():
        per_op[key] = value / divisor

    return {"available": True, "metrics": metrics, "per_op": per_op}
