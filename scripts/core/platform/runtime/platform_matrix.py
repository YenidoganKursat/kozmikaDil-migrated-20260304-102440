#!/usr/bin/env python3
"""Unified CPU/GPU platform matrix report for platform_support portability."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Dict, List

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from gpu_backend_catalog import detect_gpu_backend, resolve_gpu_backends  # noqa: E402
from target_catalog import get_target_spec, list_presets, resolve_targets  # noqa: E402


def run(command: List[str], cwd: Path) -> tuple[int, str]:
    proc = subprocess.run(
        command,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout


def summarize_cpu(records: List[Dict[str, object]]) -> Dict[str, int]:
    return {
        "total": len(records),
        "stable": sum(1 for r in records if r.get("tier") == "stable"),
        "experimental": sum(1 for r in records if r.get("tier") == "experimental"),
        "embedded": sum(1 for r in records if r.get("tier") == "embedded"),
        "build_attempted": sum(1 for r in records if r.get("build_attempted", False)),
        "build_ok": sum(1 for r in records if r.get("build_ok", False)),
        "skipped": sum(1 for r in records if not r.get("build_attempted", False)),
        "interpret_attempted": sum(
            int((r.get("interpret", {}) if isinstance(r.get("interpret"), dict) else {}).get("attempted", False))
            for r in records
        ),
        "interpret_ok": sum(
            int((r.get("interpret", {}) if isinstance(r.get("interpret"), dict) else {}).get("status_ok", False))
            for r in records
        ),
    }


def summarize_gpu(records: List[Dict[str, object]]) -> Dict[str, int]:
    return {
        "total": len(records),
        "stable": sum(1 for r in records if r.get("tier") == "stable"),
        "experimental": sum(1 for r in records if r.get("tier") == "experimental"),
        "planning": sum(1 for r in records if r.get("tier") == "planning"),
        "available": sum(1 for r in records if r.get("available", False)),
        "unavailable": sum(1 for r in records if not r.get("available", False)),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--targets", default="", help="comma-separated explicit targets")
    parser.add_argument("--preset", choices=list_presets(), default="market")
    parser.add_argument("--include-experimental", action="store_true")
    parser.add_argument("--include-embedded", action="store_true")
    parser.add_argument("--include-gpu-experimental", action="store_true")
    parser.add_argument("--include-gpu-planning", action="store_true")
    parser.add_argument("--run-multiarch-probe", action="store_true")
    parser.add_argument("--multiarch-lto", choices=["off", "thin", "full"], default="off")
    parser.add_argument("--json-out", default="bench/results/platform_support_platform_matrix.json")
    args = parser.parse_args()

    root = SCRIPT_DIR.parents[3]
    explicit_targets = [item.strip() for item in args.targets.split(",") if item.strip()]
    cpu_targets = resolve_targets(
        explicit_targets=explicit_targets if explicit_targets else None,
        preset=args.preset if not explicit_targets else None,
        include_experimental=args.include_experimental,
        include_embedded=args.include_embedded,
    )

    cpu_records: List[Dict[str, object]] = []
    for triple in cpu_targets:
        spec = get_target_spec(triple)
        cpu_records.append(
            {
                "target": triple,
                "family": spec.family if spec else "unknown",
                "os_class": spec.os_class if spec else "unknown",
                "tier": spec.tier if spec else "unknown",
                "mode": spec.mode if spec else "unknown",
                "notes": spec.notes if spec else "",
                "build_attempted": False,
                "build_ok": False,
            }
        )

    probe_output = ""
    if args.run_multiarch_probe and cpu_targets:
        probe_json = root / "bench/results/platform_support_platform_matrix_multiarch_probe.json"
        cmd = [
            "python3",
            str(root / "scripts/core/platform/runtime/multiarch_build.py"),
            "--targets",
            ",".join(cpu_targets),
            "--lto",
            args.multiarch_lto,
            "--run-interpret-smoke",
            "--json-out",
            str(probe_json.relative_to(root)),
        ]
        status, probe_output = run(cmd, cwd=root)
        if status == 0 and probe_json.exists():
            probe_payload = json.loads(probe_json.read_text(encoding="utf-8"))
            probe_by_target = {item["target"]: item for item in probe_payload.get("targets", [])}
            for row in cpu_records:
                probe = probe_by_target.get(row["target"])
                if not probe:
                    continue
                row["build_attempted"] = bool(probe.get("build_attempted", True))
                row["build_ok"] = bool(probe.get("build_ok", False))
                row["effective_target"] = probe.get("effective_target", row["target"])
                row["fallback_used"] = bool(probe.get("fallback_used", False))
                row["interpret"] = probe.get("interpret", {})
                row["smoke_attempted"] = bool(probe.get("smoke", {}).get("attempted", False))
                row["smoke_ok"] = bool(probe.get("smoke", {}).get("status_ok", False))
        else:
            for row in cpu_records:
                row["probe_error"] = "multiarch_probe_failed"

    gpu_specs = resolve_gpu_backends(
        include_experimental=args.include_gpu_experimental,
        include_planning=args.include_gpu_planning,
    )
    gpu_records = [detect_gpu_backend(spec) for spec in gpu_specs]

    cpu_summary = summarize_cpu(cpu_records)
    gpu_summary = summarize_gpu(gpu_records)

    payload = {
        "host": {
            "system": platform.system(),
            "machine": platform.machine(),
            "platform": platform.platform(),
        },
        "config": {
            "preset": args.preset if not explicit_targets else "",
            "explicit_targets": explicit_targets,
            "include_experimental": bool(args.include_experimental),
            "include_embedded": bool(args.include_embedded),
            "include_gpu_experimental": bool(args.include_gpu_experimental),
            "include_gpu_planning": bool(args.include_gpu_planning),
            "run_multiarch_probe": bool(args.run_multiarch_probe),
            "multiarch_lto": args.multiarch_lto,
        },
        "cpu": {
            "summary": cpu_summary,
            "targets": cpu_records,
            "probe_output": probe_output,
        },
        "gpu": {
            "summary": gpu_summary,
            "backends": gpu_records,
        },
    }

    out = root / args.json_out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    attempted = int(cpu_summary["build_attempted"])
    attempted_text = f"{cpu_summary['build_ok']}/{attempted}" if attempted > 0 else "n/a(no_probe)"
    print(
        "platform_support platform matrix: "
        f"cpu={attempted_text} "
        f"(total={cpu_summary['total']}), gpu_available={gpu_summary['available']}/{gpu_summary['total']}"
    )
    print(f"results json: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
