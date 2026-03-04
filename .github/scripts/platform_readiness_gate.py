#!/usr/bin/env python3
"""Portability coverage guard for platform support CPU/GPU catalog.

This guard enforces that the project keeps a broad, explicit platform matrix.
It does not claim runtime kernels are complete for every target yet; it checks
that coverage metadata and detection/reporting infrastructure stay intact.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
PLATFORM_SCRIPT_DIR = REPO_ROOT / "scripts" / "core" / "platform" / "runtime"
if str(PLATFORM_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(PLATFORM_SCRIPT_DIR))

from gpu_backend_catalog import GPU_BACKENDS
from target_catalog import TARGET_SPECS


PLATFORM_MATRIX = REPO_ROOT / "scripts" / "core" / "platform" / "runtime" / "platform_matrix.py"


def expected_cpu_target_count() -> int:
    return len(TARGET_SPECS)


def expected_gpu_backend_count() -> int:
    return len(GPU_BACKENDS)


def expected_cpu_targets() -> list[str]:
    return [item.triple for item in TARGET_SPECS]


def expected_gpu_backends() -> list[str]:
    return [item.name for item in GPU_BACKENDS]


def run_platform_matrix(json_out: Path) -> None:
    out_arg = str(json_out)
    if not json_out.is_absolute():
        out_arg = str(json_out.relative_to(REPO_ROOT))
    cmd = [
        "python3",
        str(PLATFORM_MATRIX),
        "--preset",
        "market",
        "--include-experimental",
        "--include-embedded",
        "--include-gpu-experimental",
        "--include-gpu-planning",
        "--json-out",
        out_arg,
    ]
    proc = subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    sys.stdout.write(proc.stdout)
    if proc.returncode != 0:
        raise SystemExit(f"platform_matrix command failed with status {proc.returncode}")


def _expect(cond: bool, message: str, errors: list[str]) -> None:
    if not cond:
        errors.append(message)


def validate_payload(payload: dict[str, object], *, min_cpu_targets: int, min_gpu_backends: int) -> list[str]:
    errors: list[str] = []
    cpu = payload.get("cpu", {})
    gpu = payload.get("gpu", {})
    cpu_summary = cpu.get("summary", {}) if isinstance(cpu, dict) else {}
    gpu_summary = gpu.get("summary", {}) if isinstance(gpu, dict) else {}
    cpu_targets = cpu.get("targets", []) if isinstance(cpu, dict) else []
    gpu_backends = gpu.get("backends", []) if isinstance(gpu, dict) else []

    _expect(isinstance(cpu_targets, list), "cpu.targets must be a list", errors)
    _expect(isinstance(gpu_backends, list), "gpu.backends must be a list", errors)
    if not isinstance(cpu_targets, list) or not isinstance(gpu_backends, list):
        return errors

    _expect(int(cpu_summary.get("total", 0)) >= min_cpu_targets, f"cpu total < {min_cpu_targets}", errors)
    _expect(int(cpu_summary.get("stable", 0)) >= 2, "cpu stable target count dropped below 2", errors)
    _expect(int(gpu_summary.get("total", 0)) >= min_gpu_backends, f"gpu total < {min_gpu_backends}", errors)
    _expect(int(gpu_summary.get("stable", 0)) >= 1, "gpu stable backend count dropped below 1", errors)
    _expect(int(gpu_summary.get("planning", 0)) >= 1, "gpu planning backend count dropped below 1", errors)
    cpu_ids = [item.get("target", "") for item in cpu_targets if isinstance(item, dict)]
    gpu_ids = [item.get("name", "") for item in gpu_backends if isinstance(item, dict)]
    _expect(len(gpu_ids) == len(expected_gpu_backends()), "all known gpu backends must be represented", errors)
    _expect(len(cpu_ids) == len(expected_cpu_targets()), "cpu ids list changed during probe", errors)
    _expect(len(cpu_ids) == len(set(cpu_ids)), "duplicate cpu targets in matrix report", errors)
    _expect(len(gpu_ids) == len(set(gpu_ids)), "duplicate gpu backends in matrix report", errors)
    for triple in expected_cpu_targets():
        _expect(triple in cpu_ids, f"missing required cpu target: {triple}", errors)
    _expect("metal" in gpu_ids, "missing required gpu backend: metal", errors)
    _expect("webgpu" in gpu_ids, "missing required gpu backend: webgpu", errors)

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Platform support readiness guard")
    parser.add_argument("--json-out", default="bench/results/ci_platform_matrix.json")
    parser.add_argument("--min-cpu-targets", type=int, default=0, help="override minimum cpu catalog coverage")
    parser.add_argument("--min-gpu-backends", type=int, default=0, help="override minimum gpu backend coverage")
    args = parser.parse_args()

    json_out = REPO_ROOT / args.json_out
    json_out.parent.mkdir(parents=True, exist_ok=True)
    run_platform_matrix(json_out)

    if args.min_cpu_targets <= 0:
        args.min_cpu_targets = max(1, expected_cpu_target_count())
    if args.min_gpu_backends <= 0:
        args.min_gpu_backends = max(1, expected_gpu_backend_count())

    payload = json.loads(json_out.read_text(encoding="utf-8"))
    errors = validate_payload(
        payload,
        min_cpu_targets=args.min_cpu_targets,
        min_gpu_backends=args.min_gpu_backends,
    )

    if errors:
        print("platform_readiness_gate: FAIL")
        for item in errors:
            print(f"  - {item}")
        return 1

    cpu_summary = payload.get("cpu", {}).get("summary", {})
    gpu_summary = payload.get("gpu", {}).get("summary", {})
    print(
        "platform_readiness_gate: PASS "
        f"(cpu total={cpu_summary.get('total', 0)}, stable={cpu_summary.get('stable', 0)}; "
        f"gpu total={gpu_summary.get('total', 0)}, stable={gpu_summary.get('stable', 0)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
