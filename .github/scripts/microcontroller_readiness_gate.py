#!/usr/bin/env python3
"""Embedded microcontroller readiness gate for CI and audit runs."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
PLATFORM_RUNTIME_SCRIPTS = REPO_ROOT / "scripts" / "core" / "platform" / "runtime"
MULTIARCH_SCRIPT = REPO_ROOT / "scripts" / "core" / "platform" / "runtime" / "multiarch_build.py"
REFERENCE_PROGRAM = "bench/programs/scalar_runtime/scalar_sum.k"

if str(PLATFORM_RUNTIME_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(PLATFORM_RUNTIME_SCRIPTS))

from target_catalog import get_target_spec, resolve_targets  # noqa: E402


EXPECTED_MICRO_TARGETS = (
    "arm-none-eabi",
    "riscv64-unknown-elf",
    "avr-none-elf",
    "xtensa-esp32-elf",
)


def run_multiarch_embedded(json_out: Path, *, run_interpret_smoke: bool) -> dict[str, Any]:
    out_arg = str(json_out)
    if json_out.is_relative_to(REPO_ROOT):
        out_arg = str(json_out.relative_to(REPO_ROOT))

    cmd = [
        "python3",
        str(MULTIARCH_SCRIPT),
        "--preset",
        "embedded",
        "--include-embedded",
        "--program",
        REFERENCE_PROGRAM,
        "--json-out",
        out_arg,
    ]
    if run_interpret_smoke:
        cmd.append("--run-interpret-smoke")
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
        raise SystemExit(f"multiarch_build failed with status {proc.returncode}")

    payload = json.loads(json_out.read_text(encoding="utf-8"))
    return payload


def validate_embedded_payload(
    payload: dict[str, object],
    *,
    require_native: bool,
    require_interpret_smoke: bool,
) -> tuple[bool, list[str], list[str], dict[str, object]]:
    errors: list[str] = []
    warnings: list[str] = []
    targets = payload.get("targets", [])
    if not isinstance(targets, list):
        return (
            False,
            ["payload.targets must be a list"],
            [],
            {},
        )

    by_target: dict[str, dict[str, object]] = {}
    for entry in targets:
        if not isinstance(entry, dict):
            continue
        by_target[str(entry.get("target", ""))] = entry

    for triple in EXPECTED_MICRO_TARGETS:
        if triple not in by_target:
            errors.append(f"embedded target missing from multiarch report: {triple}")

    native_ready = 0
    interpret_ready = 0
    interpret_checked = 0

    for triple, entry in by_target.items():
        if triple not in EXPECTED_MICRO_TARGETS:
            continue
        spec = get_target_spec(triple)
        if spec is None:
            errors.append(f"catalog entry missing: {triple}")
            continue
        if spec.tier != "embedded":
            errors.append(f"unexpected tier for embedded target {triple}: {spec.tier}")
        interpret = entry.get("interpret", {}) if isinstance(entry.get("interpret"), dict) else {}
        if entry.get("mode") == "interpret_only":
            if entry.get("build_attempted"):
                errors.append(f"embedded target {triple} was unexpectedly build-attempted in interpret_only mode")
            if entry.get("build_ok"):
                errors.append(f"embedded target {triple} has impossible build_ok=true in interpret_only mode")
            if not interpret.get("attempted"):
                if require_interpret_smoke:
                    errors.append(f"embedded target {triple} requires interpret smoke but it was not run")
                else:
                    warnings.append(f"embedded target {triple} interpret smoke not executed")
            elif not interpret.get("status_ok"):
                errors.append(f"embedded target {triple} failed interpret smoke validation")
            else:
                interpret_ready += 1
            interpret_checked += int(bool(interpret.get("attempted", False)))
            continue

        if spec.mode != "aot":
            errors.append(f"embedded target {triple} has non-interpretable plan outside known mode: {spec.mode}")

        if not entry.get("build_attempted"):
            msg = f"embedded target {triple} expected AOT attempt in native mode"
            if require_native:
                errors.append(msg)
            else:
                warnings.append(msg)
        if not entry.get("build_ok"):
            msg = f"embedded target {triple} native build not yet passing"
            if require_native:
                errors.append(msg)
            else:
                warnings.append(msg)
        else:
            native_ready += 1

        output = str(entry.get("output", ""))
        if output and "skipped" in output:
            warnings.append(f"embedded target {triple} unexpectedly reported skipped output in non-interpret mode")

    summary = {
        "embedded_targets": sorted(by_target.keys()),
        "plan_modes": sorted({str(by_target[t].get("mode", "unknown")) for t in EXPECTED_MICRO_TARGETS if t in by_target}),
        "buildable": native_ready,
        "skipped": sum(1 for entry in by_target.values() if not entry.get("build_attempted", False)),
        "interpret_checked": interpret_checked,
        "interpret_ok": interpret_ready,
        "report_rows": by_target,
    }
    return len(errors) == 0, errors, warnings, summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Microcontroller readiness gate")
    parser.add_argument("--json-out", default="bench/results/platform_support_microcontroller_gate.json")
    parser.add_argument(
        "--require-native-mcu",
        action="store_true",
        help="treat embedded targets as must-build/ready (no interpret-only fallback)",
    )
    parser.add_argument(
        "--require-interpret-smoke",
        action="store_true",
        help="require --run-interpret-smoke for interpret_only embedded targets",
    )
    args = parser.parse_args()

    json_out = REPO_ROOT / args.json_out
    json_out.parent.mkdir(parents=True, exist_ok=True)

    embedded_targets = resolve_targets(
        explicit_targets=None,
        preset="embedded",
        include_experimental=True,
        include_embedded=True,
    )
    if len(embedded_targets) < len(EXPECTED_MICRO_TARGETS):
        print("microcontroller_readiness_gate: FAIL")
        print(f"  - embedded preset too small: expected >= {len(EXPECTED_MICRO_TARGETS)}, got {len(embedded_targets)}")
        return 1

    payload = run_multiarch_embedded(json_out, run_interpret_smoke=args.require_interpret_smoke)
    payload_targets = payload.get("targets", [])
    if not isinstance(payload_targets, list):
        print("microcontroller_readiness_gate: FAIL")
        print("  - missing or invalid targets list in multiarch payload")
        return 1

    passed, errors, warnings, summary = validate_embedded_payload(
        payload,
        require_native=args.require_native_mcu,
        require_interpret_smoke=args.require_interpret_smoke,
    )
    if errors:
        print("microcontroller_readiness_gate: FAIL")
        for item in errors:
            print(f"  - {item}")
        payload["microcontroller_gate"] = {"pass": False, "errors": errors, "summary": summary}
        json_out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        return 1

    print("microcontroller_readiness_gate: PASS")
    print(f"  - micro targets: {', '.join(summary['embedded_targets'])}")
    print(f"  - buildable: {summary['buildable']}")
    print(f"  - interpret smoke passed/checked: {summary['interpret_ok']}/{summary['interpret_checked']}")
    if warnings:
        print("  - warnings:")
        for item in warnings:
            print(f"    - {item}")
    payload["microcontroller_gate"] = {
        "pass": True,
        "summary": summary,
        "warnings": warnings,
    }
    json_out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
