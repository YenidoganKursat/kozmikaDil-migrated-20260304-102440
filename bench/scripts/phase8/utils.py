import statistics
import subprocess
import time
from pathlib import Path
from typing import Dict, List, Tuple


def run_once(command: List[str], env=None):
    start = time.perf_counter()
    proc = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        env=env,
    )
    end = time.perf_counter()
    return proc.returncode, proc.stdout, proc.stderr, end - start


def collect_timing_samples(command: List[str], env, runs: int, warmup_runs: int):
    return collect_timing_samples_repeated(
        command=command,
        env=env,
        runs=runs,
        warmup_runs=warmup_runs,
        sample_repeat=1,
    )


def collect_timing_samples_repeated(
    command: List[str],
    env,
    runs: int,
    warmup_runs: int,
    sample_repeat: int,
):
    for _ in range(max(0, warmup_runs)):
        for _ in range(max(1, sample_repeat)):
            run_once(command, env=env)

    statuses = []
    outputs = []
    times = []
    for idx in range(max(1, runs)):
        start = time.perf_counter()
        stdout = ""
        for rep in range(max(1, sample_repeat)):
            status, current_stdout, _, _ = run_once(command, env=env)
            statuses.append(status)
            if idx == 0 and rep == 0:
                stdout = current_stdout
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        if idx == 0:
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
    if median_time > 0.0:
        drifts = [abs((x - median_time) / median_time) * 100.0 for x in trimmed]
        max_drift = max(drifts) if drifts else 0.0
    else:
        max_drift = 0.0

    return {
        "sample_count": len(samples),
        "trimmed_count": len(trimmed),
        "median_time_sec": median_time,
        "mean_time_sec": mean_time,
        "stdev_time_sec": stdev_time,
        "max_drift_percent": max_drift,
        "reproducible": bool(max_drift <= drift_limit_percent),
    }


def _as_bool(token: str) -> bool:
    return token.strip().lower() in {"true", "1", "pass", "ok"}


def parse_phase8_output(output: str) -> Dict[str, float]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(lines) < 14:
        raise ValueError(f"phase8 output expected >=14 lines, got {len(lines)}")

    return {
        "total": float(lines[0]),
        "expected": float(lines[1]),
        "diff": float(lines[2]),
        "pass": 1.0 if _as_bool(lines[3]) else 0.0,
        "calls": float(lines[4]),
        "own_calls": float(lines[5]),
        "blas_calls": float(lines[6]),
        "cache_hit_a": float(lines[7]),
        "cache_hit_b": float(lines[8]),
        "epilogue_fused": float(lines[9]),
        "tile_m": float(lines[10]),
        "tile_n": float(lines[11]),
        "tile_k": float(lines[12]),
        "backend_id": float(lines[13]),
    }


def parse_c_baseline_output(output: str) -> Dict[str, float]:
    result: Dict[str, float] = {}
    for raw in output.splitlines():
        line = raw.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if key == "pass":
            result[key] = 1.0 if _as_bool(value) else 0.0
            continue
        try:
            result[key] = float(value)
        except ValueError:
            continue
    if "total" not in result or "expected" not in result:
        raise ValueError("failed to parse C baseline output")
    return result


def compile_c_baseline(c_path: Path, out_path: Path, c_compiler: str, c_flags: str) -> Tuple[bool, str]:
    cmd = [c_compiler, *c_flags.split(), str(c_path), "-o", str(out_path)]
    status, _, stderr, _ = run_once(cmd)
    if status != 0:
        return False, stderr.strip()
    return True, ""


def within_tolerance(actual: float, expected: float, eps: float = 1e-9) -> bool:
    scale = max(1.0, abs(expected))
    return abs(actual - expected) <= eps * scale
