#!/usr/bin/env python3
"""Repository architecture and coverage guard for mandatory src-root migration.

Hard rules:
- Canonical source root is src/ (+ include/)
- No phase* directory naming in src/test/scripts
- Port boundary is reachable only from src/core/driver
- DTO construction patterns outside driver are forbidden
- Legacy workflow path markers are forbidden
"""

from __future__ import annotations

import argparse
import pathlib
import re
from dataclasses import dataclass

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

FILE_SIZE_BUDGET_EXCEPTIONS = {
    pathlib.Path("src/core/manager/typecheck/semantic_runtime/evaluator_parts/stmt/stmt_while.cpp"),
    pathlib.Path("src/core/logic/runtime/scalar_runtime/runtime/primitives/numeric_scalar_core_parts/01_mpfr_and_cast.cpp"),
    pathlib.Path("src/core/servis/codegen/lowering/codegen_parts/03_ir_to_c.cpp"),
}


@dataclass
class Finding:
    level: str
    message: str
    path: pathlib.Path | None = None


def load_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def line_count(path: pathlib.Path) -> int:
    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        return sum(1 for _ in handle)


def check_required_directories(findings: list[Finding]) -> None:
    required = [
        "src/core",
        "src/core/dto",
        "src/core/behavior",
        "src/core/mapper",
        "src/core/driver",
        "src/core/manager",
        "src/core/servis",
        "src/core/logic",
        "src/core/mode",
        "src/core/ui",
        "src/core/common",
        "src/port/input",
        "src/port/output",
        "src/application",
        "src/application/wiring",
        "include/spark",
        "include/spark/core",
        "test/core/pipeline/smoke",
        "test/core/pipeline/parsing",
        "test/core/pipeline/semantic_runtime",
        "test/core/pipeline/lowering",
        "test/core/pipeline/containers_primitives",
        "test/core/pipeline/hetero_runtime",
        "test/core/pipeline/stream_pipeline",
        "test/core/pipeline/matmul_schedule_gpu",
        "test/core/pipeline/async_runtime",
        "test/core/pipeline/platform_support",
        "scripts/core/platform/runtime",
    ]
    for rel in required:
        path = REPO_ROOT / rel
        if not path.exists():
            findings.append(Finding("error", f"missing required directory: {rel}", path))


def check_no_phase_named_directories(findings: list[Finding]) -> None:
    phase_dir = re.compile(r"^phase[0-9]+$")
    roots = [REPO_ROOT / "src", REPO_ROOT / "test", REPO_ROOT / "scripts"]
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_dir():
                continue
            if phase_dir.match(path.name):
                findings.append(Finding("error", "phase-named directory is forbidden", path))


def check_no_phase_tokens_in_code(findings: list[Finding]) -> None:
    roots = [
        REPO_ROOT / "src",
        REPO_ROOT / "include",
        REPO_ROOT / "test",
        REPO_ROOT / "scripts",
        REPO_ROOT / ".github" / "workflows",
        REPO_ROOT / ".github" / "scripts",
    ]
    exts = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".py", ".sh", ".yml", ".yaml", ".md", ".k", ".txt"}
    token = re.compile(r"phase[0-9]+")
    skipped_prefixes = ("doc/", "record/", "bench/results/", "build/", ".build_")
    allowed_files = {
        pathlib.Path("include/spark/cpu_features.h"),
        pathlib.Path("src/core/common/cpu_features.cpp"),
        pathlib.Path(".github/scripts/architecture_guard.py"),
    }

    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(REPO_ROOT)
            rel_s = rel.as_posix()
            if rel_s.startswith(skipped_prefixes):
                continue
            if path.suffix not in exts and path.name != "CMakeLists.txt":
                continue
            text = load_text(path)
            if path.suffix in {".yml", ".yaml"}:
                text = text.replace("workflow_dispatch", "")
            matches = token.findall(text)
            if not matches:
                continue
            if rel in allowed_files:
                continue
            findings.append(Finding("error", f"phase token forbidden in source/config: {rel}", path))


def check_architecture_docs(findings: list[Finding]) -> None:
    path = REPO_ROOT / "doc" / "architecture" / "core_port_application_model.md"
    if not path.exists():
        findings.append(Finding("error", "missing architecture model doc", path))
        return
    text = load_text(path)
    required_markers = [
        "src/core",
        "src/port",
        "src/application",
        "Port ile konuşan tek core katmanı driver",
        "DTO construct/new sadece driver",
    ]
    for marker in required_markers:
        if marker not in text:
            findings.append(Finding("error", f"architecture doc marker missing: {marker}", path))


def check_driver_only_port_contact(findings: list[Finding]) -> None:
    core_root = REPO_ROOT / "src" / "core"
    if not core_root.exists():
        return
    extensions = {".cpp", ".cc", ".cxx", ".h", ".hpp"}
    patterns = [
        re.compile(r"\bport[/\\](input|output)\b"),
        re.compile(r"#include\s*[\"<].*spark/port[/\\]"),
        re.compile(r"\bport::(input|output)::"),
    ]
    for path in core_root.rglob("*"):
        if not path.is_file() or path.suffix not in extensions:
            continue
        rel = path.relative_to(core_root)
        if rel.parts and rel.parts[0] == "driver":
            continue
        text = load_text(path)
        for pattern in patterns:
            if pattern.search(text):
                findings.append(Finding("error", f"non-driver core layer references port boundary: {rel}", path))
                break


def check_dependency_direction(findings: list[Finding]) -> None:
    core_root = REPO_ROOT / "src" / "core"
    if not core_root.exists():
        return

    rank = {
        "dto": 1,
        "behavior": 1,
        "mapper": 2,
        "driver": 3,
        "manager": 4,
        "servis": 5,
        "logic": 6,
        "mode": 7,
        "ui": 8,
        "common": 0,
    }

    include_pattern = re.compile(r'#include\s*["<]spark/core/([^/]+)/')

    for layer, layer_rank in rank.items():
        if layer in {"common"}:
            continue
        layer_root = core_root / layer
        if not layer_root.exists():
            continue
        for path in layer_root.rglob("*"):
            if not path.is_file() or path.suffix not in {".cpp", ".h", ".hpp"}:
                continue
            text = load_text(path)
            for dep in include_pattern.findall(text):
                if dep not in rank:
                    continue
                dep_rank = rank[dep]
                if dep_rank > layer_rank:
                    findings.append(
                        Finding(
                            "error",
                            f"dependency direction violation: {layer} -> {dep}",
                            path,
                        )
                    )


def check_dto_construct_outside_driver(findings: list[Finding]) -> None:
    core_root = REPO_ROOT / "src" / "core"
    if not core_root.exists():
        return

    patterns = [
        re.compile(r"\bnew\s+dto::[A-Za-z_][A-Za-z0-9_]*\b"),
        re.compile(r"=\s*dto::[A-Za-z_][A-Za-z0-9_]*\s*\{"),
        re.compile(r"return\s+dto::[A-Za-z_][A-Za-z0-9_]*\s*\{"),
    ]

    for path in core_root.rglob("*"):
        if not path.is_file() or path.suffix not in {".cpp", ".h", ".hpp"}:
            continue
        rel = path.relative_to(core_root)
        if rel.parts and rel.parts[0] == "driver":
            continue
        text = load_text(path)
        for pattern in patterns:
            if pattern.search(text):
                findings.append(Finding("error", f"dto construct pattern outside driver: {rel}", path))
                break


def check_compiler_alias_layout(findings: list[Finding]) -> None:
    expected_links = {
        REPO_ROOT / "compiler" / "src" / "core": "../../src/core",
        REPO_ROOT / "compiler" / "src" / "port": "../../src/port",
        REPO_ROOT / "compiler" / "src" / "application": "../../src/application",
        REPO_ROOT / "compiler" / "include" / "spark": "../../include/spark",
    }
    for path, target in expected_links.items():
        if not path.exists():
            findings.append(Finding("error", "missing compatibility alias", path))
            continue
        if not path.is_symlink():
            findings.append(Finding("error", "compatibility alias must be symlink", path))
            continue
        if path.readlink().as_posix() != target:
            findings.append(Finding("error", f"compatibility alias target mismatch: expected {target}", path))


def check_no_legacy_runtime_stdlib_roots(findings: list[Finding]) -> None:
    legacy_dirs = [
        REPO_ROOT / "runtime" / "src",
        REPO_ROOT / "runtime" / "include",
        REPO_ROOT / "stdlib" / "src",
        REPO_ROOT / "stdlib" / "include",
    ]
    for path in legacy_dirs:
        if not path.exists():
            continue
        has_files = any(item.is_file() for item in path.rglob("*"))
        if has_files:
            findings.append(Finding("error", "legacy runtime/stdlib production code root is forbidden", path))


def check_root_cmake_no_legacy_subdirs(findings: list[Finding]) -> None:
    root_cmake = REPO_ROOT / "CMakeLists.txt"
    if not root_cmake.exists():
        findings.append(Finding("error", "missing root CMakeLists.txt", root_cmake))
        return
    text = load_text(root_cmake)
    forbidden = [
        "add_subdirectory(runtime)",
        "add_subdirectory(stdlib)",
    ]
    for marker in forbidden:
        if marker in text:
            findings.append(Finding("error", f"forbidden legacy root CMake subdirectory marker: {marker}", root_cmake))


def check_no_core_pipeline_alias(findings: list[Finding]) -> None:
    alias_root = REPO_ROOT / "src" / "core" / "pipeline"
    if alias_root.exists():
        findings.append(Finding("error", "src/core/pipeline compatibility alias is forbidden", alias_root))


def check_file_size_budget(findings: list[Finding], max_lines: int) -> None:
    roots = [REPO_ROOT / "src", REPO_ROOT / "include", REPO_ROOT / "test", REPO_ROOT / "bench" / "scripts"]
    exts = {".cpp", ".h", ".hpp", ".py"}
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in exts:
                continue
            rel = path.relative_to(REPO_ROOT)
            if rel in FILE_SIZE_BUDGET_EXCEPTIONS:
                continue
            lines = line_count(path)
            if lines > max_lines:
                findings.append(Finding("error", f"file exceeds line budget ({lines} > {max_lines}): {rel}", path))


def check_primitive_coverage(findings: list[Finding]) -> None:
    families = ["i8", "i16", "i32", "i64", "i128", "i256", "i512", "f8", "f16", "f32", "f64", "f128", "f256", "f512"]
    coverage_files = [
        REPO_ROOT / "test" / "core" / "pipeline" / "containers_primitives" / "primitives" / "primitive_numeric_extreme_tests.cpp",
        REPO_ROOT / "test" / "core" / "pipeline" / "semantic_runtime" / "eval_tests.cpp",
        REPO_ROOT / "test" / "core" / "pipeline" / "containers_primitives" / "primitives" / "crosslang_native_primitives_tests.py",
    ]
    merged = ""
    for path in coverage_files:
        if not path.exists():
            findings.append(Finding("error", "primitive coverage source missing", path))
            continue
        merged += "\n" + load_text(path)

    for primitive in families:
        if not re.search(rf"\b{re.escape(primitive)}\b", merged):
            findings.append(Finding("error", f"primitive family token not covered: {primitive}"))
    for op in ["+", "-", "*", "/", "%", "^"]:
        if op not in merged:
            findings.append(Finding("error", f"numeric operator token missing from coverage set: {op}"))


def check_ci_wiring(findings: list[Finding]) -> None:
    workflows = list((REPO_ROOT / ".github" / "workflows").glob("*.yml"))
    if not workflows:
        findings.append(Finding("error", "missing CI workflows", REPO_ROOT / ".github" / "workflows"))
        return

    merged = ""
    for w in workflows:
        merged += "\n" + load_text(w)

    phase_word = "phase"
    forbidden = [
        "scripts/" + phase_word + "10/",
        "scripts/core/pipeline/" + phase_word + "10/",
        "test/" + phase_word,
        "sparkc_" + phase_word,
        "bench/programs/" + phase_word,
        "bench/results/" + phase_word,
        "compiler" + "/src",
        "compiler" + "/include",
    ]
    for marker in forbidden:
        if marker in merged:
            findings.append(Finding("error", f"forbidden legacy path marker in workflows: {marker}"))

    required_markers = [
        "architecture_guard.py",
        "precision_policy_guard.py",
        "platform_readiness_gate.py",
        "perf_regression_gate.py",
        "ctest --test-dir",
        "scripts/core/platform/runtime/gpu_backend_runtime_perf.py",
        "scripts/core/platform/runtime/gpu_smoke_matrix.py",
    ]
    for marker in required_markers:
        if marker not in merged:
            findings.append(Finding("error", f"CI marker missing: {marker}"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Architecture and coverage guard")
    parser.add_argument("--max-lines", type=int, default=1800)
    args = parser.parse_args()

    findings: list[Finding] = []
    check_required_directories(findings)
    check_no_phase_named_directories(findings)
    check_no_phase_tokens_in_code(findings)
    check_architecture_docs(findings)
    check_driver_only_port_contact(findings)
    check_dependency_direction(findings)
    check_dto_construct_outside_driver(findings)
    check_compiler_alias_layout(findings)
    check_no_legacy_runtime_stdlib_roots(findings)
    check_root_cmake_no_legacy_subdirs(findings)
    check_no_core_pipeline_alias(findings)
    check_file_size_budget(findings, args.max_lines)
    check_primitive_coverage(findings)
    check_ci_wiring(findings)

    errors = [f for f in findings if f.level == "error"]
    warnings = [f for f in findings if f.level == "warning"]
    phase_token_errors = [f for f in errors if "phase token forbidden" in f.message]

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
        f"max_lines={args.max_lines} phase_token_errors={len(phase_token_errors)}"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
