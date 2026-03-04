#!/usr/bin/env python3
"""GPU backend smoke matrix for platform_support portability checks.

This script is host-aware:
- it probes backend toolchains/drivers using the backend catalog,
- it can run non-strict (report-only) mode for generic CI hosts,
- it can run strict mode for selected backends on dedicated GPU runners.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[3]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from gpu_backend_catalog import (
    GPU_BACKENDS,
    canonicalize_backend_name,
    detect_gpu_backend,
    known_backend_names,
    normalize_backend_list,
)


def parse_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description="GPU backend smoke matrix")
    parser.add_argument(
        "--backends",
        default="all",
        help="comma-separated backend names or 'all'",
    )
    parser.add_argument(
        "--include-planning",
        action="store_true",
        help="include planning-only backends in report",
    )
    parser.add_argument(
        "--fail-on-unavailable",
        default="",
        help="comma-separated backend names that must be available on this host",
    )
    parser.add_argument(
        "--json-out",
        default="bench/results/platform_support_gpu_smoke.json",
    )
    args = parser.parse_args()

    selected: list[str]
    if args.backends.strip().lower() == "all":
        selected = known_backend_names()
    else:
        selected = normalize_backend_list(parse_csv(args.backends))

    known = set(known_backend_names())
    unknown = [item for item in selected if canonicalize_backend_name(item) not in known]
    if unknown:
        raise SystemExit(f"unknown backend(s): {', '.join(unknown)}")

    fail_set = set(normalize_backend_list(parse_csv(args.fail_on_unavailable)))
    unknown_fail = [item for item in fail_set if item not in known]
    if unknown_fail:
        raise SystemExit(f"unknown backend(s) in --fail-on-unavailable: {', '.join(unknown_fail)}")

    records = []
    for spec in GPU_BACKENDS:
        if spec.name not in selected:
            continue
        if spec.tier == "planning" and not args.include_planning:
            continue
        row = detect_gpu_backend(spec)
        records.append(row)
        print(
            f"[gpu-smoke] {row['name']}: "
            f"available={row['available']} tier={row['tier']} reason={row['reason']}"
        )

    by_name = {item["name"]: item for item in records}
    failing_required = []
    for name in sorted(fail_set):
        row = by_name.get(name)
        if not row:
            failing_required.append(name)
            continue
        if row.get("mode") == "planning_only":
            continue
        if not bool(row.get("available", False)):
            failing_required.append(name)
    passed = len(failing_required) == 0

    payload = {
        "selected": selected,
        "include_planning": bool(args.include_planning),
        "required_available": sorted(fail_set),
        "passed": passed,
        "failing_required": failing_required,
        "records": records,
    }

    out = REPO_ROOT / args.json_out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"gpu_smoke json: {out}")

    if not passed:
        print(f"gpu_smoke FAIL: required backend unavailable: {', '.join(failing_required)}")
        return 1
    print("gpu_smoke PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
