#!/usr/bin/env python3
import argparse
import csv
import json
import math
import os
from pathlib import Path
import sys
from typing import Dict, List


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent.parent
K_BIN = ROOT_DIR / "k"
PROGRAM_DIR = ROOT_DIR / "bench" / "programs" / "phase9"
RESULTS_DIR = ROOT_DIR / "bench" / "results"

sys.path.insert(0, str(SCRIPT_DIR / "phase9"))
from definitions import phase9_chunk_sweep, phase9_thread_sweep  # noqa: E402
from utils import collect_timing_samples, parse_numeric_lines, summarize_times  # noqa: E402


def _run_program(source: str, expected_lines: int, args, env_extra=None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    command = [str(K_BIN), "run", "--interpret", str(PROGRAM_DIR / source)]
    statuses, first_output, samples = collect_timing_samples(
        command=command,
        env=env,
        runs=args.runs,
        warmup_runs=args.warmup_runs,
        sample_repeat=args.sample_repeat,
    )
    timing = summarize_times(samples, args.drift_limit)
    status_ok = all(code == 0 for code in statuses)
    parsed = []
    parse_error = ""
    if status_ok:
        try:
            parsed = parse_numeric_lines(first_output, expected_lines)
        except Exception as exc:  # noqa: BLE001
            status_ok = False
            parse_error = str(exc)
    reproducible_ok = (not args.require_reproducible) or bool(timing.get("reproducible", 0.0))
    return {
        "status_ok": status_ok,
        "reproducible_ok": reproducible_ok,
        "statuses": statuses,
        "timing": timing,
        "parsed": parsed,
        "parse_error": parse_error,
        "output": first_output,
    }


def run_spawn_join(args):
    record = _run_program("spawn_join_overhead.k", 4, args)
    checksum_ok = False
    counters_ok = False
    ns_per_task = 0.0
    allocs_per_task_est = 0.0
    if record["status_ok"]:
        checksum, n, spawned, executed = record["parsed"]
        expected = n * (n + 1.0) / 2.0
        checksum_ok = math.isclose(checksum, expected, rel_tol=1e-9, abs_tol=1e-9)
        counters_ok = spawned >= n and executed >= n
        median = record["timing"]["median_time_sec"]
        if n > 0:
            ns_per_task = (median * 1e9) / n
            allocs_per_task_est = spawned / n

    passed = bool(record["status_ok"] and record["reproducible_ok"] and checksum_ok and counters_ok)
    return {
        "name": "spawn_join_overhead",
        "pass": passed,
        "checksum_ok": checksum_ok,
        "counters_ok": counters_ok,
        "ns_per_task": ns_per_task,
        "allocs_per_task_est": allocs_per_task_est,
        **record,
    }


def run_channel(args):
    record = _run_program("channel_throughput.k", 4, args)
    checksum_ok = False
    counters_ok = False
    throughput_msg_per_s = 0.0
    latency_ns = 0.0
    allocs_per_msg_est = 0.0
    if record["status_ok"]:
        checksum, n, send_count, recv_count = record["parsed"]
        expected = n * (n + 1.0) / 2.0
        checksum_ok = math.isclose(checksum, expected, rel_tol=1e-9, abs_tol=1e-9)
        counters_ok = send_count == n and recv_count == n
        median = record["timing"]["median_time_sec"]
        if median > 0.0:
            throughput_msg_per_s = n / median
        if n > 0:
            latency_ns = (median * 1e9) / n
            allocs_per_msg_est = send_count / n

    passed = bool(record["status_ok"] and record["reproducible_ok"] and checksum_ok and counters_ok)
    return {
        "name": "channel_throughput",
        "pass": passed,
        "checksum_ok": checksum_ok,
        "counters_ok": counters_ok,
        "throughput_msg_per_s": throughput_msg_per_s,
        "latency_ns": latency_ns,
        "allocs_per_msg_est": allocs_per_msg_est,
        **record,
    }


def run_parallel_scaling(args):
    cpu = max(1, os.cpu_count() or 1)
    threads = [t for t in phase9_thread_sweep() if t <= cpu]
    if 1 not in threads:
        threads = [1] + threads
    runs: List[Dict] = []
    checksum_ref = None
    time_ref = 0.0

    for t in threads:
        rec = _run_program("parallel_for_scaling.k", 6, args, env_extra={"SPARK_PHASE9_THREADS": str(t)})
        parsed_ok = rec["status_ok"]
        checksum_ok = False
        counters_ok = False
        allocs_per_iter_est = 0.0
        speedup_vs_1 = 0.0
        efficiency = 0.0
        if parsed_ok:
            checksum, n, threads_reported, spawned, executed, steals = rec["parsed"]
            if checksum_ref is None:
                checksum_ref = checksum
            checksum_ok = math.isclose(checksum, checksum_ref, rel_tol=1e-9, abs_tol=1e-9)
            counters_ok = (threads_reported >= 1) and (spawned >= 1) and (executed >= 1)
            if n > 0:
                allocs_per_iter_est = spawned / n
            median = rec["timing"]["median_time_sec"]
            if t == 1:
                time_ref = median
            if time_ref > 0.0 and median > 0.0:
                speedup_vs_1 = time_ref / median
                efficiency = speedup_vs_1 / max(1, t)
        runs.append(
            {
                "threads": t,
                "pass": bool(parsed_ok and rec["reproducible_ok"] and checksum_ok and counters_ok),
                "checksum_ok": checksum_ok,
                "counters_ok": counters_ok,
                "speedup_vs_1": speedup_vs_1,
                "efficiency": efficiency,
                "allocs_per_iter_est": allocs_per_iter_est,
                **rec,
            }
        )

    scaling_values = [row["speedup_vs_1"] for row in runs if row["threads"] > 1]
    scaling_ok = (not scaling_values) or (max(scaling_values) > 1.1)
    passed = all(row["pass"] for row in runs) and scaling_ok
    return {
        "name": "parallel_for_scaling",
        "pass": passed,
        "scaling_ok": scaling_ok,
        "runs": runs,
    }


def run_chunk_sweep(args):
    runs: List[Dict] = []
    checksum_ref = None
    best_chunk = 0
    best_time = 0.0
    for chunk in phase9_chunk_sweep():
        rec = _run_program("par_reduce_chunk.k", 2, args, env_extra={"SPARK_PHASE9_CHUNK": str(chunk)})
        checksum_ok = False
        if rec["status_ok"]:
            checksum, length = rec["parsed"]
            _ = length
            if checksum_ref is None:
                checksum_ref = checksum
            checksum_ok = math.isclose(checksum, checksum_ref, rel_tol=1e-9, abs_tol=1e-9)
            median = rec["timing"]["median_time_sec"]
            if best_chunk == 0 or (median > 0.0 and median < best_time):
                best_chunk = chunk
                best_time = median
        runs.append(
            {
                "chunk": chunk,
                "pass": bool(rec["status_ok"] and rec["reproducible_ok"] and checksum_ok),
                "checksum_ok": checksum_ok,
                **rec,
            }
        )
    passed = all(row["pass"] for row in runs)
    return {
        "name": "par_reduce_chunk",
        "pass": passed,
        "best_chunk": best_chunk,
        "best_time_sec": best_time,
        "runs": runs,
    }


def write_results(config, spawn_record, channel_record, scaling_record, chunk_record):
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULTS_DIR / "phase9_benchmarks.json"
    csv_path = RESULTS_DIR / "phase9_benchmarks.csv"

    payload = {
        "config": config,
        "spawn_join": spawn_record,
        "channel": channel_record,
        "parallel_scaling": scaling_record,
        "chunk_sweep": chunk_record,
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["section", "name", "key", "value"])
        writer.writerow(["spawn_join", "spawn_join_overhead", "pass", int(spawn_record["pass"])])
        writer.writerow(["spawn_join", "spawn_join_overhead", "ns_per_task", spawn_record["ns_per_task"]])
        writer.writerow(
            ["spawn_join", "spawn_join_overhead", "allocs_per_task_est", spawn_record["allocs_per_task_est"]]
        )

        writer.writerow(["channel", "channel_throughput", "pass", int(channel_record["pass"])])
        writer.writerow(
            ["channel", "channel_throughput", "throughput_msg_per_s", channel_record["throughput_msg_per_s"]]
        )
        writer.writerow(["channel", "channel_throughput", "latency_ns", channel_record["latency_ns"]])
        writer.writerow(
            ["channel", "channel_throughput", "allocs_per_msg_est", channel_record["allocs_per_msg_est"]]
        )

        writer.writerow(["parallel_scaling", "parallel_for_scaling", "pass", int(scaling_record["pass"])])
        writer.writerow(["parallel_scaling", "parallel_for_scaling", "scaling_ok", int(scaling_record["scaling_ok"])])
        for run in scaling_record["runs"]:
            writer.writerow(["parallel_scaling", "parallel_for_scaling", f"threads_{run['threads']}_speedup", run["speedup_vs_1"]])
            writer.writerow(
                ["parallel_scaling", "parallel_for_scaling", f"threads_{run['threads']}_efficiency", run["efficiency"]]
            )
            writer.writerow(
                ["parallel_scaling", "parallel_for_scaling", f"threads_{run['threads']}_allocs_per_iter_est", run["allocs_per_iter_est"]]
            )

        writer.writerow(["chunk_sweep", "par_reduce_chunk", "pass", int(chunk_record["pass"])])
        writer.writerow(["chunk_sweep", "par_reduce_chunk", "best_chunk", chunk_record["best_chunk"]])
        writer.writerow(["chunk_sweep", "par_reduce_chunk", "best_time_sec", chunk_record["best_time_sec"]])

    return json_path, csv_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--sample-repeat", type=int, default=3)
    parser.add_argument("--drift-limit", type=float, default=3.0)
    parser.add_argument("--require-reproducible", action="store_true")
    args = parser.parse_args()

    config = {
        "runs": args.runs,
        "warmup_runs": args.warmup_runs,
        "sample_repeat": args.sample_repeat,
        "drift_limit": args.drift_limit,
        "require_reproducible": args.require_reproducible,
    }

    spawn_record = run_spawn_join(args)
    channel_record = run_channel(args)
    scaling_record = run_parallel_scaling(args)
    chunk_record = run_chunk_sweep(args)
    json_path, csv_path = write_results(config, spawn_record, channel_record, scaling_record, chunk_record)

    total_sections = 4
    passed_sections = sum(
        int(item["pass"]) for item in [spawn_record, channel_record, scaling_record, chunk_record]
    )
    print(f"phase9 sections: {passed_sections}/{total_sections} passed")
    print(f"results json: {json_path}")
    print(f"results csv: {csv_path}")


if __name__ == "__main__":
    main()
