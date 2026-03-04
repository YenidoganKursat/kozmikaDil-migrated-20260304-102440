#!/usr/bin/env python3
"""Strict precision policy guard.

Policy enforced:
- no relaxed floating-point compile flags in build/runtime wiring
- strict FP defaults stay enabled in runtime kernels
"""

from __future__ import annotations

import pathlib
import re
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
STRICT_FLAGS_SOURCE = (
    REPO_ROOT
    / "src"
    / "core"
    / "driver"
    / "parsing"
    / "bootstrap"
    / "sparkc_main_parts"
    / "01_common.cpp"
)
MATMUL_KERNEL_SOURCE = (
    REPO_ROOT
    / "src"
    / "core"
    / "logic"
    / "schedule"
    / "schedule_gpu"
    / "runtime"
    / "03_matmul_kernel.cpp"
)

SCAN_PATHS = [
    REPO_ROOT / "CMakeLists.txt",
    REPO_ROOT / "compiler" / "CMakeLists.txt",
    MATMUL_KERNEL_SOURCE,
    REPO_ROOT / ".github" / "workflows" / "ci.yml",
    REPO_ROOT / "bench" / "scripts",
    REPO_ROOT / "scripts",
]

FORBIDDEN_FLAG_PATTERNS = [
    r"(^|[\s\"'])-ffast-math($|[\s\"'])",
    r"(^|[\s\"'])-Ofast($|[\s\"'])",
    r"(^|[\s\"'])-funsafe-math-optimizations($|[\s\"'])",
    r"(^|[\s\"'])-fassociative-math($|[\s\"'])",
    r"(^|[\s\"'])-freciprocal-math($|[\s\"'])",
    r"(^|[\s\"'])-fno-signed-zeros($|[\s\"'])",
]


def load_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def iter_files() -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for p in SCAN_PATHS:
        if not p.exists():
            continue
        if p.is_file():
            files.append(p)
            continue
        for entry in p.rglob("*"):
            if entry.is_file() and entry.suffix in {".py", ".sh", ".cpp", ".h", ".hpp", ".yml", ".yaml", ".txt"}:
                files.append(entry)
    return files


def main() -> int:
    errors: list[str] = []
    files = iter_files()

    compiled_patterns = [re.compile(p) for p in FORBIDDEN_FLAG_PATTERNS]

    for path in files:
        text = load_text(path)
        for pattern in compiled_patterns:
            if pattern.search(text):
                rel = path.relative_to(REPO_ROOT)
                errors.append(f"forbidden relaxed-FP flag found in {rel}: {pattern.pattern}")

    if not STRICT_FLAGS_SOURCE.exists():
        errors.append(f"missing strict compiler flag source: {STRICT_FLAGS_SOURCE.relative_to(REPO_ROOT)}")
    else:
        strict_flags_source = load_text(STRICT_FLAGS_SOURCE)
        if "enforce_strict_precision_compiler_flags(flags);" not in strict_flags_source:
            errors.append("missing strict compiler flag sanitizer call in canonical build flags source")
        if "-fno-fast-math" not in strict_flags_source:
            errors.append("missing -fno-fast-math in native compiler flag policy")
        for forbidden in [
            "-ffast-math",
            "-Ofast",
            "-funsafe-math-optimizations",
            "-fassociative-math",
            "-freciprocal-math",
            "-fno-signed-zeros",
        ]:
            marker = f'append_if_missing(flags, "{forbidden}")'
            if marker in strict_flags_source:
                errors.append(f"forbidden relaxed-FP flag is appended in native flag resolver: {forbidden}")

    if not MATMUL_KERNEL_SOURCE.exists():
        errors.append(f"missing matmul strict-fp source: {MATMUL_KERNEL_SOURCE.relative_to(REPO_ROOT)}")
    else:
        matmul_source = load_text(MATMUL_KERNEL_SOURCE)
        if 'env_flag_enabled("SPARK_MATMUL_STRICT_FP", false)' in matmul_source:
            errors.append("matmul strict FP default is relaxed (false); must be strict")

    if errors:
        for err in errors:
            print(f"[ERROR] {err}")
        print(f"precision_policy_guard_summary errors={len(errors)}")
        return 1

    print("precision_policy_guard_summary errors=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
