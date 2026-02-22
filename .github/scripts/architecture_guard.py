#!/usr/bin/env python3
"""Repository architecture and test coverage guard for CI.

This guard is intentionally pragmatic:
- fail on missing core layer/test directories,
- fail on oversized implementation files (maintainability risk),
- fail if primitive family coverage drops from key tests/scripts,
- fail if CI workflow stops invoking ctest/cross-language/perf gates.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


@dataclass
class Finding:
    level: str
    message: str
    path: pathlib.Path | None = None


def load_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def line_count(path: pathlib.Path) -> int:
    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        return sum(1 for _ in handle)


def check_required_directories(findings: list[Finding]) -> None:
    required = [
        "compiler/src/common",
        "compiler/src/phase1",
        "compiler/src/phase2",
        "compiler/src/phase3",
        "compiler/src/phase4",
        "compiler/src/phase5",
        "compiler/src/phase6",
        "compiler/src/phase7",
        "compiler/src/phase8",
        "compiler/src/phase9",
        "tests/phase1",
        "tests/phase2",
        "tests/phase3",
        "tests/phase4",
        "tests/phase5",
        "tests/phase6",
        "tests/phase7",
        "tests/phase8",
        "tests/phase9",
        "tests/phase10",
    ]
    for rel in required:
        path = REPO_ROOT / rel
        if not path.exists():
            findings.append(Finding("error", f"missing required directory: {rel}", path))


def check_file_size_budget(findings: list[Finding], max_lines: int) -> None:
    roots = [
        REPO_ROOT / "compiler" / "src",
        REPO_ROOT / "compiler" / "include",
        REPO_ROOT / "tests",
        REPO_ROOT / "bench" / "scripts",
    ]
    extensions = {".cpp", ".h", ".hpp", ".py"}

    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in extensions:
                continue
            lines = line_count(path)
            if lines > max_lines:
                rel = path.relative_to(REPO_ROOT)
                findings.append(
                    Finding(
                        "error",
                        f"file exceeds line budget ({lines} > {max_lines}): {rel}",
                        path,
                    )
                )


def check_primitive_coverage(findings: list[Finding]) -> None:
    families = [
        "i8",
        "i16",
        "i32",
        "i64",
        "i128",
        "i256",
        "i512",
        "f8",
        "f16",
        "f32",
        "f64",
        "f128",
        "f256",
        "f512",
    ]

    coverage_files = [
        REPO_ROOT / "tests" / "phase5" / "primitives" / "primitive_numeric_extreme_tests.cpp",
        REPO_ROOT / "tests" / "phase3" / "eval_tests.cpp",
        REPO_ROOT / "tests" / "phase5" / "primitives" / "crosslang_native_primitives_tests.py",
    ]
    merged = ""
    for path in coverage_files:
        if not path.exists():
            findings.append(Finding("error", "primitive coverage source missing", path))
            continue
        merged += "\n" + load_text(path)

    for primitive in families:
        pattern = re.compile(rf"\b{re.escape(primitive)}\b")
        if not pattern.search(merged):
            findings.append(Finding("error", f"primitive family token not covered: {primitive}"))

    for op in ["+", "-", "*", "/", "%", "^"]:
        if op not in merged:
            findings.append(Finding("error", f"numeric operator token missing from coverage set: {op}"))


def check_ci_wiring(findings: list[Finding]) -> None:
    ci = REPO_ROOT / ".github" / "workflows" / "ci.yml"
    if not ci.exists():
        findings.append(Finding("error", "missing CI workflow", ci))
        return
    text = load_text(ci)

    required_markers = [
        "ctest --test-dir build",
        "crosslang_native_primitives_tests.py",
        "perf_regression_gate.py",
        "--repeat until-fail:3",
    ]
    for marker in required_markers:
        if marker not in text:
            findings.append(Finding("error", f"CI marker missing: {marker}", ci))


def main() -> int:
    parser = argparse.ArgumentParser(description="Architecture and coverage guard")
    parser.add_argument("--max-lines", type=int, default=1800)
    args = parser.parse_args()

    findings: list[Finding] = []
    check_required_directories(findings)
    check_file_size_budget(findings, args.max_lines)
    check_primitive_coverage(findings)
    check_ci_wiring(findings)

    errors = [f for f in findings if f.level == "error"]
    warnings = [f for f in findings if f.level == "warning"]

    for finding in findings:
        prefix = finding.level.upper()
        if finding.path:
            try:
                rel = finding.path.relative_to(REPO_ROOT)
            except ValueError:
                rel = finding.path
            print(f"[{prefix}] {finding.message} ({rel})")
        else:
            print(f"[{prefix}] {finding.message}")

    print(
        f"architecture_guard_summary errors={len(errors)} warnings={len(warnings)} "
        f"max_lines={args.max_lines}"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
