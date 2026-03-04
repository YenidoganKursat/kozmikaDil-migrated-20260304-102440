#!/usr/bin/env python3
"""Cross-platform performance smoke for CPU/MCU targets in the catalog."""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from target_catalog import get_target_spec, list_presets, resolve_targets  # noqa: E402


ROOT_DIR = SCRIPT_DIR.parents[3]
K_BIN = ROOT_DIR / "k"


def run_command(
    command: Sequence[str],
    cwd: Path,
    timeout_sec: float,
    env: Dict[str, str] | None = None,
) -> Tuple[int, str, float]:
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            command,
            cwd=str(cwd),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            timeout=timeout_sec,
        )
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return proc.returncode, proc.stdout, elapsed_ms
    except subprocess.TimeoutExpired as exc:  # noqa: BLE001
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return 124, str(exc), elapsed_ms
    except Exception as exc:  # noqa: BLE001
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        return 1, str(exc), elapsed_ms


def parse_first_numeric(text: str) -> Tuple[bool, float]:
    for line in text.splitlines():
        token = line.strip()
        if not token:
            continue
        try:
            return True, float(token)
        except ValueError:
            continue
    return False, 0.0


def summarize_samples(samples_ms: Sequence[float]) -> Dict[str, float]:
    if not samples_ms:
        return {"median_ms": 0.0, "mean_ms": 0.0, "min_ms": 0.0, "max_ms": 0.0, "p95_ms": 0.0}
    sorted_samples = sorted(samples_ms)
    p95_idx = int(max(0, min(len(sorted_samples) - 1, round((len(sorted_samples) - 1) * 0.95))))
    return {
        "median_ms": round(float(statistics.median(sorted_samples)), 6),
        "mean_ms": round(float(statistics.fmean(sorted_samples)), 6),
        "min_ms": round(sorted_samples[0], 6),
        "max_ms": round(sorted_samples[-1], 6),
        "p95_ms": round(float(sorted_samples[p95_idx]), 6),
        "runs": len(sorted_samples),
    }


def run_and_profile(
    command: Sequence[str],
    runs: int,
    warmup: int,
    timeout_sec: float,
    cwd: Path,
) -> Dict[str, Any]:
    warmup_runs = max(0, warmup)
    for _ in range(warmup_runs):
        run_command(command, cwd, timeout_sec)

    samples_ms: List[float] = []
    statuses: List[int] = []
    passed = 0
    output = ""
    parsed_ok = False

    for i in range(max(1, runs)):
        status, out, elapsed_ms = run_command(command, cwd, timeout_sec)
        statuses.append(status)
        samples_ms.append(elapsed_ms)
        if i == 0:
            parsed_ok, _ = parse_first_numeric(out)
            output = out

    stats = summarize_samples(samples_ms) if samples_ms else {}
    stats.update(
        {
            "attempted": True,
            "status_ok": all(item == 0 for item in statuses),
            "passed": all(item == 0 for item in statuses) and parsed_ok,
            "status_last": statuses[-1] if statuses else 1,
            "output": output,
            "checks": stats.pop("runs", 0),
        }
    )
    stats["sample_count"] = len(samples_ms)
    return stats


def host_arch() -> str:
    import platform as _platform

    machine = _platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x86_64"
    if machine in {"i386", "i486", "i586", "i686", "x86"}:
        return "x86"
    if machine in {"arm64", "aarch64"}:
        return "aarch64"
    if machine.startswith("riscv64"):
        return "riscv64"
    return machine


def target_arch(token: str) -> str:
    return token.split("-", maxsplit=1)[0]


def is_host_target(target: str) -> bool:
    return normalize_token(target_arch(target)) == host_arch()


def normalize_token(value: str) -> str:
    arch = value.lower()
    if arch in {"x86_64", "amd64"}:
        return "x86_64"
    if arch in {"i386", "i486", "i586", "i686", "x86", "x32"}:
        return "x86"
    if arch in {"arm64", "aarch64"}:
        return "aarch64"
    if arch.startswith("riscv64"):
        return "riscv64"
    if arch.startswith("riscv32"):
        return "riscv32"
    if arch in {"ppc64", "ppc64le"}:
        return "ppc64le"
    if arch in {"ppc", "powerpc"}:
        return "ppc"
    return arch


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a performance smoke across CPU/MCU targets.")
    parser.add_argument("--program", default="bench/programs/platform_support/perf_micro_loop.k")
    parser.add_argument("--targets", default="")
    parser.add_argument("--preset", choices=list_presets(), default="market")
    parser.add_argument("--all-targets", action="store_true", help="include experimental and embedded targets")
    parser.add_argument("--include-experimental", action="store_true")
    parser.add_argument("--include-embedded", action="store_true")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--run-build", action="store_true")
    parser.add_argument("--run-host-exec", action="store_true")
    parser.add_argument("--json-out", default="bench/results/platform_support_cross_platform_perf.json")
    args = parser.parse_args()

    program = ROOT_DIR / args.program
    if not program.exists():
        raise SystemExit(f"missing program: {program}")

    explicit_targets = [item.strip() for item in args.targets.split(",") if item.strip()]
    include_experimental = args.include_experimental or args.all_targets
    include_embedded = args.include_embedded or args.all_targets
    targets = resolve_targets(
        explicit_targets=explicit_targets if explicit_targets else None,
        preset=args.preset if not explicit_targets else None,
        include_experimental=include_experimental,
        include_embedded=include_embedded,
    )
    if not targets:
        raise SystemExit("no targets resolved")

    os_summary: Dict[str, Dict[str, float]] = {}
    records: List[Dict[str, Any]] = []

    interpret_pass = 0
    interpret_attempted = 0
    build_pass = 0
    build_attempted = 0
    host_exec_pass = 0
    host_exec_attempted = 0

    for target in targets:
        spec = get_target_spec(target)
        mode = spec.mode if spec else "aot"
        os_class = spec.os_class if spec else "unknown"

        interpret_stats: Dict[str, Any] = {"attempted": False, "passed": False}
        build_stats: Dict[str, Any] = {"attempted": False, "passed": False}
        host_exec_stats: Dict[str, Any] = {"attempted": False, "passed": False}

        interpret_command = [str(K_BIN), "run", "--interpret", str(program), "--target", target]
        interpret_stats = run_and_profile(
            command=interpret_command,
            runs=args.runs,
            warmup=args.warmup,
            timeout_sec=args.timeout,
            cwd=ROOT_DIR,
        )
        interpret_stats["command"] = interpret_command
        interpret_attempted += 1
        if interpret_stats["passed"]:
            interpret_pass += 1

        target_summary = os_summary.setdefault(
            os_class,
            {
                "targets": 0,
                "interpret_pass": 0,
                "interpret_attempted": 0,
            },
        )
        target_summary["targets"] += 1
        target_summary["interpret_attempted"] += 1
        if interpret_stats["passed"]:
            target_summary["interpret_pass"] += 1

        if args.run_build and mode != "interpret_only":
            build_attempted += 1
            build_stats["attempted"] = True
            build_output_path = ROOT_DIR / "build" / "platform_support" / "perf_smoke" / f"{target}.bin"
            build_output_path.parent.mkdir(parents=True, exist_ok=True)
            build_command = [str(K_BIN), "build", str(program), "-o", str(build_output_path), "--target", target]
            build_stats["command"] = build_command
            build_status, build_out, build_ms = run_command(build_command, ROOT_DIR, args.timeout)
            build_stats["status"] = build_status
            build_stats["status_ok"] = build_status == 0
            build_stats["output"] = build_out
            build_stats["build_ms"] = build_ms
            if build_status == 0:
                build_pass += 1
                build_stats["passed"] = True

                if args.run_host_exec and is_host_target(target):
                    host_exec_attempted += 1
                    host_exec_stats["attempted"] = True
                    host_exec_stats.update(
                        run_and_profile(
                            command=[str(build_output_path)],
                            runs=args.runs,
                            warmup=args.warmup,
                            timeout_sec=args.timeout,
                            cwd=ROOT_DIR,
                        )
                    )
                    if host_exec_stats.get("passed"):
                        host_exec_pass += 1

        records.append(
            {
                "target": target,
                "family": spec.family if spec else "unknown",
                "os_class": os_class,
                "tier": spec.tier if spec else "unknown",
                "mode": mode,
                "interpret": interpret_stats,
                "build": build_stats,
                "host_exec": host_exec_stats,
            }
        )

    for class_stats in os_summary.values():
        attempts = class_stats["interpret_attempted"] or 1
        class_stats["interpret_pass_rate"] = round(class_stats["interpret_pass"] / attempts, 4)

    payload = {
        "config": {
            "program": str(program),
            "preset": args.preset if not explicit_targets else "",
            "targets": targets,
            "explicit_targets": explicit_targets,
            "include_experimental": bool(include_experimental),
            "include_embedded": bool(include_embedded),
            "runs": max(1, args.runs),
            "warmup": max(0, args.warmup),
            "timeout": float(args.timeout),
            "run_build": bool(args.run_build),
            "run_host_exec": bool(args.run_host_exec),
            "host_platform": {"system": platform.system(), "machine": platform.machine()},
        },
        "summary": {
            "interpret_attempted": interpret_attempted,
            "interpret_pass": interpret_pass,
            "build_attempted": build_attempted,
            "build_pass": build_pass,
            "host_exec_attempted": host_exec_attempted,
            "host_exec_pass": host_exec_pass,
        },
        "os_summary": os_summary,
        "targets": records,
    }

    out_json = ROOT_DIR / args.json_out
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print(
        f"platform_support cross perf: interpret={interpret_pass}/{interpret_attempted} "
        f"(targets={len(targets)}), build={build_pass}/{build_attempted}, "
        f"host_exec={host_exec_pass}/{host_exec_attempted}"
    )
    print(f"results json: {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
