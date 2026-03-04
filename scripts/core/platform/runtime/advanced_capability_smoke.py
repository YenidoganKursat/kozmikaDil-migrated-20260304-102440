#!/usr/bin/env python3
"""Run an advanced capability probe across all catalog CPU + MCU targets."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from target_catalog import get_target_spec, list_presets, resolve_targets  # noqa: E402


ROOT_DIR = SCRIPT_DIR.parents[3]
K_BIN = ROOT_DIR / "k"


def run(command: List[str], cwd: Path) -> Tuple[int, str]:
    proc = subprocess.run(
        command,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout


def normalize_arch_token(raw: str) -> str:
    arch = raw.lower()
    if arch in {"x86_64", "amd64"}:
        return "x86_64"
    if arch in {"i386", "i486", "i586", "i686", "x86", "x32"}:
        return "x86"
    if arch in {"arm64", "aarch64"}:
        return "aarch64"
    if arch in {"armv7", "armv7a", "armv7l"}:
        return "armv7"
    if arch.startswith("riscv64"):
        return "riscv64"
    if arch.startswith("riscv32"):
        return "riscv32"
    if arch in {"ppc64", "ppc64le"}:
        return "ppc64le"
    if arch in {"ppc", "powerpc"}:
        return "ppc"
    if arch == "s390x":
        return "s390x"
    if arch == "loongarch64":
        return "loongarch64"
    if arch == "mips64":
        return "mips64"
    if arch in {"mips", "mipsel"}:
        return "mips"
    if arch == "wasm32":
        return "wasm32"
    if arch == "wasm64":
        return "wasm64"
    return arch


def host_arch() -> str:
    machine = platform.machine().lower()
    return normalize_arch_token(machine)


def is_host_target(target: str) -> bool:
    return normalize_arch_token(target.split("-", maxsplit=1)[0]) == host_arch()


def parse_output_checks(output: str) -> tuple[List[int], Dict[str, float]]:
    values: List[int] = []
    floats: Dict[str, float] = {}
    for line in output.splitlines():
        text = line.strip()
        if not text:
            continue
        try:
            value = float(text)
        except ValueError:
            continue
        values.append(int(value))
        floats[str(len(values) - 1)] = value
    return values, floats


def build_probe_row(target: str, program: Path, run_build: bool, run_host_exec: bool) -> Dict[str, Any]:
    spec = get_target_spec(target)
    mode = spec.mode if spec else "aot"

    build = {
        "attempted": False,
        "status": 0,
        "passed": False,
        "status_ok": False,
        "command": [],
        "output": "",
        "binary": "",
    }

    host_exec = {
        "attempted": False,
        "status": 0,
        "passed": False,
        "status_ok": False,
        "command": [],
        "output": "",
        "checks": [],
    }

    target_output = {
        "attempted": True,
        "status": 0,
        "passed": False,
        "status_ok": False,
        "command": [],
        "output": "",
        "checks": [],
        "check_count": 0,
    }

    # Interpreter smoke is universal and deterministic for portability gating.
    interpret_command = [str(K_BIN), "run", "--interpret", str(program), "--target", target]
    target_output["command"] = interpret_command
    interpret_status, interpret_output = run(interpret_command, cwd=ROOT_DIR)
    target_output["status"] = interpret_status
    target_output["status_ok"] = interpret_status == 0
    checks, values = parse_output_checks(interpret_output)
    target_output["output"] = interpret_output
    target_output["checks"] = checks[:6]
    target_output["raw_values"] = values
    target_output["check_count"] = len(checks)
    target_output["passed"] = False
    if target_output["status_ok"] and target_output["check_count"] >= 6:
        target_output["passed"] = all(item == 1 for item in checks[:6])

    # Build smoke for non-embedded/native-ready targets.
    if run_build and mode != "interpret_only":
        build["attempted"] = True
        out_bin = ROOT_DIR / "build" / "platform_support" / "advanced_capability_smoke" / f"{target}.bin"
        out_bin.parent.mkdir(parents=True, exist_ok=True)
        build_command = [str(K_BIN), "build", str(program), "-o", str(out_bin), "--target", target]
        build["command"] = build_command
        build["binary"] = str(out_bin)
        build_status, build_output = run(build_command, cwd=ROOT_DIR)
        build["status"] = build_status
        build["status_ok"] = build_status == 0
        build["output"] = build_output
        build["passed"] = build["status_ok"]

        # Host run only when binary can be executed locally.
        if run_host_exec and build["status_ok"] and is_host_target(target):
            host_exec["attempted"] = True
            host_exec["command"] = [str(out_bin)]
            host_status, host_output = run([str(out_bin)], cwd=ROOT_DIR)
            host_exec["status"] = host_status
            host_exec["status_ok"] = host_status == 0
            host_exec["output"] = host_output
            host_checks, _ = parse_output_checks(host_output)
            host_exec["checks"] = host_checks[:6]
            host_exec["passed"] = host_exec["status_ok"] and len(host_checks) >= 6 and all(
                item == 1 for item in host_checks[:6]
            )
    else:
        build["status"] = 0
        build["output"] = ""

    return {
        "target": target,
        "mode": mode,
        "tier": spec.tier if spec else "unknown",
        "target_output": target_output,
        "build": build,
        "host_exec": host_exec,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", default="bench/programs/platform_support/advanced_capability_probe.k")
    parser.add_argument("--targets", default="")
    parser.add_argument("--preset", choices=list_presets(), default="market")
    parser.add_argument(
        "--all-targets",
        action="store_true",
        help="include experimental + embedded targets from the catalog",
    )
    parser.add_argument("--include-experimental", action="store_true")
    parser.add_argument("--include-embedded", action="store_true")
    parser.add_argument("--run-build", action="store_true")
    parser.add_argument("--run-host-exec", action="store_true")
    parser.add_argument("--json-out", default="bench/results/platform_support_advanced_capability_smoke.json")
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

    records: List[Dict[str, Any]] = []
    for target in targets:
        records.append(
            build_probe_row(
                target=target,
                program=program,
                run_build=args.run_build,
                run_host_exec=args.run_host_exec,
            )
        )

    interpret_pass = sum(1 for item in records if item["target_output"]["passed"])
    build_pass = sum(1 for item in records if item["build"].get("passed"))
    host_exec_pass = sum(1 for item in records if item["host_exec"]["passed"])

    payload = {
        "config": {
            "program": str(program),
            "preset": args.preset if not explicit_targets else "",
            "explicit_targets": explicit_targets,
            "include_experimental": bool(include_experimental),
            "include_embedded": bool(include_embedded),
            "run_build": bool(args.run_build),
            "run_host_exec": bool(args.run_host_exec),
            "target_count": len(targets),
        },
        "summary": {
            "interpret_attempted": len(targets),
            "interpret_pass": interpret_pass,
            "build_attempted": sum(1 for item in records if item["build"]["attempted"]),
            "build_pass": build_pass,
            "host_exec_attempted": sum(1 for item in records if item["host_exec"]["attempted"]),
            "host_exec_pass": host_exec_pass,
        },
        "targets": records,
    }

    summary = payload["summary"]
    print(f"platform_support advanced probe: interpret={summary['interpret_pass']}/{summary['interpret_attempted']}")
    print(f"platform_support advanced probe: build={summary['build_pass']}/{summary['build_attempted']}")
    print(f"platform_support advanced probe: host_exec={summary['host_exec_pass']}/{summary['host_exec_attempted']}")
    out_json = ROOT_DIR / args.json_out
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"results json: {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
