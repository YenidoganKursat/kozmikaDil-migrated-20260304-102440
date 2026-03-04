#!/usr/bin/env python3
"""Unified policy gate for CPU/GPU/microcontroller portability support checks."""

from __future__ import annotations

import argparse
import json
import platform
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
PLATFORM_RUNTIME_DIR = REPO_ROOT / "scripts" / "core" / "platform" / "runtime"

if str(PLATFORM_RUNTIME_DIR) not in sys.path:
    sys.path.insert(0, str(PLATFORM_RUNTIME_DIR))

from target_catalog import TARGET_SPECS, get_target_spec  # noqa: E402


def _default_for_host(prefix: str) -> str:
    return f"bench/results/{prefix}_{platform.system()}.json"


def _legacy_fallback_path(path: Path) -> Path | None:
    text = path.as_posix()
    if "platform_support_" in text:
        legacy_prefix = "phase" + "10_"
        return Path(text.replace("platform_support_", legacy_prefix))
    return None


def _host_suffix_fallback_path(path: Path) -> Path | None:
    stem = path.stem
    for suffix in ("_Darwin", "_Linux", "_Windows"):
        if stem.endswith(suffix):
            return path.with_name(stem[: -len(suffix)] + path.suffix)
    return None


def _load_json(path: Path) -> Dict[str, Any]:
    if not path.exists():
        fallback_candidates = [
            _host_suffix_fallback_path(path),
            _legacy_fallback_path(path),
        ]
        for candidate in fallback_candidates:
            if candidate is not None and candidate.exists():
                path = candidate
                break
        else:
            raise SystemExit(f"missing json file: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def _normalize_os(value: str | None) -> str:
    return (value or platform.system()).strip()


def _normalize_arch(value: str) -> str:
    arch = (value or "").lower()
    if arch in {"x86_64", "amd64"}:
        return "x86_64"
    if arch in {"i386", "i486", "i586", "i686", "x86"}:
        return "x86"
    if arch in {"arm64", "aarch64"}:
        return "aarch64"
    if arch.startswith("riscv64"):
        return "riscv64"
    if arch.startswith("riscv32"):
        return "riscv32"
    if arch in {"ppc64", "ppc64le"}:
        return "ppc64le"
    if arch == "s390x":
        return "s390x"
    if arch in {"mips", "mipsel"}:
        return "mips"
    if arch.startswith("mips64"):
        return "mips64"
    if arch in {"loongarch64"}:
        return "loongarch64"
    return arch


def _target_triple_arch(target: str) -> str:
    return _normalize_arch(target.split("-", maxsplit=1)[0])


def _os_class_for_system(system: str) -> str:
    return {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(system, system.lower())


def _exception_targets(policy_section: Dict[str, Any], section: str, system: str) -> set[str]:
    section_cfg = policy_section.get(section, {})
    out: set[str] = set()
    for item in section_cfg.get("all", []):
        if not isinstance(item, str):
            continue
        out.add(str(item))
    for item in section_cfg.get(system, []):
        if not isinstance(item, str):
            continue
        out.add(str(item))
    return out


def _normalize_backend_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        token = value.strip()
        return [token] if token else []
    if isinstance(value, (list, tuple, set)):
        out: list[str] = []
        for item in value:
            if not isinstance(item, str):
                continue
            item = item.strip()
            if item:
                out.append(item)
        return out
    return []


def _resolve_gpu_policy_targets(policy_value: Any, system: str) -> tuple[set[str], set[str]]:
    """Return (required_backends, optional_backends) for a host."""
    required: set[str] = set()
    optional: set[str] = set()

    if not isinstance(policy_value, dict):
        return required, optional

    required_cfg = policy_value.get("required")
    optional_cfg = policy_value.get("optional")

    # New policy shape:
    #   required: {"all": [...], "Linux": [...], ...}
    #   optional: {"all": [...], "Linux": [...], ...}
    if isinstance(required_cfg, dict):
        required.update(_normalize_backend_list(required_cfg.get("all")))
        required.update(_normalize_backend_list(required_cfg.get(system)))
    elif isinstance(required_cfg, (list, tuple, set)):
        required.update(_normalize_backend_list(required_cfg))

    if isinstance(optional_cfg, dict):
        optional.update(_normalize_backend_list(optional_cfg.get("all")))
        optional.update(_normalize_backend_list(optional_cfg.get(system)))
    elif isinstance(optional_cfg, (list, tuple, set, str)):
        optional.update(_normalize_backend_list(optional_cfg))

    # Legacy policy shape:
    #   {"Linux": [...], "Darwin": [...], "Windows": [...], "all": [...]}
    if not required and not required_cfg:
        required.update(_normalize_backend_list(policy_value.get("all")))
        required.update(_normalize_backend_list(policy_value.get(system)))

    required = set(required)
    optional = set(optional) - required
    return required, optional


def _append(results: List[Dict[str, Any]], name: str, passed: bool, message: str, details: Any | None = None, level: str = "check") -> None:
    results.append(
        {
            "name": name,
            "status": "pass" if passed else "fail",
            "level": level,
            "message": message,
            "details": details,
        }
    )


def _load_policy(path: Path) -> Dict[str, Any]:
    policy = _load_json(path)
    if not isinstance(policy, dict):
        raise SystemExit("platform policy file is not a JSON object")
    return policy


def _load_cpu_rows(payload: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    cpu_obj = payload.get("cpu", {})
    rows = cpu_obj.get("targets") if isinstance(cpu_obj, dict) else payload.get("targets")
    if not isinstance(rows, list):
        return {}
    return {str(row.get("target", "")): row for row in rows if isinstance(row, dict) and row.get("target")}


def _load_advanced_rows(payload: Dict[str, Any] | None) -> Dict[str, Dict[str, Any]]:
    if not isinstance(payload, dict):
        return {}
    records = payload.get("targets")
    if not isinstance(records, list):
        return {}
    return {str(row.get("target", "")): row for row in records if isinstance(row, dict) and row.get("target")}


def _load_perf_rows(payload: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    records = payload.get("targets")
    if not isinstance(records, list):
        return {}
    return {str(row.get("target", "")): row for row in records if isinstance(row, dict) and row.get("target")}


def _load_gpu_rows(payload: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    records = payload.get("records")
    if isinstance(records, list):
        return {str(row.get("name", "")): row for row in records if isinstance(row, dict) and row.get("name")}

    gpu_obj = payload.get("gpu", {})
    backends = gpu_obj.get("backends")
    if isinstance(backends, list):
        return {str(row.get("name", "")): row for row in backends if isinstance(row, dict) and row.get("name")}

    return {}


def _load_micro_rows(payload: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    targets = payload.get("targets")
    if not isinstance(targets, list):
        return {}
    return {str(row.get("target", "")): row for row in targets if isinstance(row, dict) and row.get("target")}


def _normalize_target_list(values: Any) -> list[str]:
    if values is None:
        return []
    if isinstance(values, str):
        token = values.strip()
        return [token] if token else []
    if isinstance(values, (list, tuple, set)):
        out: list[str] = []
        for item in values:
            if not isinstance(item, str):
                continue
            token = item.strip()
            if token:
                out.append(token)
        return out
    return []


def _expand_targets(
    policy_value: Any,
    all_targets: Iterable[str],
    system: str | None = None,
) -> List[str]:
    if policy_value == "all" or policy_value is None:
        return list(all_targets)
    if isinstance(policy_value, dict):
        values = _normalize_target_list(policy_value.get("all", []))
        if system:
            values.extend(_normalize_target_list(policy_value.get(system, [])))
        if not values:
            return list(all_targets)
        out: list[str] = []
        seen: set[str] = set()
        for item in values:
            if item in seen:
                continue
            seen.add(item)
            out.append(item)
        return out
    normalized = _normalize_target_list(policy_value)
    if normalized:
        return normalized
    return list(all_targets)


def _find_host_targets(cpu_rows: Dict[str, Dict[str, Any]], system: str, host_arch: str) -> List[Dict[str, Any]]:
    host_os_class = _os_class_for_system(system)
    out: List[Dict[str, Any]] = []
    for row in cpu_rows.values():
        target = str(row.get("target", ""))
        if not target:
            continue
        spec = get_target_spec(target)
        if spec is None:
            continue
        if spec.os_class != host_os_class:
            continue
        if _target_triple_arch(target) == host_arch and str(row.get("mode", "")) != "interpret_only":
            out.append(row)
    return out


def _interpret_record(row: Dict[str, Any]) -> Dict[str, Any]:
    if "interpret" in row and isinstance(row["interpret"], dict):
        return row["interpret"]
    if "target_output" in row and isinstance(row["target_output"], dict):
        return row["target_output"]
    return {}


def evaluate_cpu_section(
    policy: Dict[str, Any],
    cpu_payload: Dict[str, Any],
    system: str,
    advanced_payload: Dict[str, Any] | None = None,
) -> Tuple[List[Dict[str, Any]], bool]:
    rows = _load_cpu_rows(cpu_payload)
    if not rows:
        return [{"name": "cpu-payload", "status": "fail", "message": "missing cpu target rows"}], False

    cfg = policy.get("targets", {})
    if not isinstance(cfg, dict):
        return [{"name": "cpu-policy", "status": "fail", "message": "missing targets policy block"}], False

    checks: List[Dict[str, Any]] = []
    ok = True
    advanced_rows = _load_advanced_rows(advanced_payload)

    all_cpu_targets = [s.triple for s in TARGET_SPECS]
    required_build_targets = _expand_targets(cfg.get("required_build_targets"), all_cpu_targets, system)
    interpret_targets = _expand_targets(cfg.get("required_interpret_targets"), all_cpu_targets, system)

    build_ex = _exception_targets(cfg.get("exceptions", {}), "build_failures", system)
    interpret_ex = _exception_targets(cfg.get("exceptions", {}), "interpret_failures", system)
    host_exec_ex = _exception_targets(cfg.get("exceptions", {}), "host_exec_failures", system)

    # Build coverage check: host-target families defined by policy must be build-capable.
    for target in required_build_targets:
        row = rows.get(target)
        if not row:
            if target in build_ex:
                _append(checks, f"cpu-build[{target}]", True, "target row missing but explicitly excepted for build", level="warn")
                continue
            _append(checks, f"cpu-build[{target}]", False, "missing target in cpu payload")
            ok = False
            continue

        if str(row.get("mode", "")) == "interpret_only":
            _append(
                checks,
                f"cpu-build[{target}]",
                True,
                "required target is interpret_only; build check is intentionally non-applicable",
                level="warn",
            )
            continue

        attempted = bool(row.get("build_attempted", False))
        passed = bool(row.get("build_ok", False))
        if (not attempted or not passed) and target not in build_ex:
            _append(checks, f"cpu-build[{target}]", False, f"build did not pass (attempted={attempted}, pass={passed})")
            ok = False
        else:
            _append(checks, f"cpu-build[{target}]", True, "build passed or allowed by exception")

    # Interpreter smoke checks for all catalog or policy-defined required targets.
    for target in interpret_targets:
        row = rows.get(target)
        if not row:
            if target in interpret_ex:
                _append(checks, f"cpu-interpret[{target}]", True, "target missing but excepted", level="warn")
                continue
            _append(checks, f"cpu-interpret[{target}]", False, "target missing in payload")
            ok = False
            continue

        interpret = _interpret_record(row)
        attempted = bool(interpret.get("attempted", False))
        passed = bool(interpret.get("status_ok", False))

        if advanced_rows:
            advanced = _interpret_record(advanced_rows.get(target, {}))
            if bool(advanced.get("attempted", False)) and bool(advanced.get("status_ok", False)):
                attempted = True
                passed = True
        if not attempted:
            message = "interpreter smoke not attempted"
            if target in interpret_ex:
                _append(checks, f"cpu-interpret[{target}]", True, f"{message} (excepted)", level="warn")
            else:
                _append(checks, f"cpu-interpret[{target}]", False, message)
                ok = False
            continue
        if not passed and target not in interpret_ex:
            _append(checks, f"cpu-interpret[{target}]", False, "interpreter smoke failed")
            ok = False
            continue
        _append(checks, f"cpu-interpret[{target}]", True, "interpreter smoke passed")

    # Optional host-runtime smoke check.
    required_host_exec = cfg.get("required_host_exec_targets", "")
    if required_host_exec == "host":
        host_arch = _normalize_arch(platform.machine())
        host_rows = _find_host_targets(rows, system, host_arch)
        if not host_rows:
            _append(checks, "cpu-host-exec", False, f"no host target candidate for arch={host_arch}, os={system}")
            ok = False
        else:
            for row in host_rows:
                target = str(row.get("target", ""))
                host_exec = row.get("smoke") if isinstance(row.get("smoke"), dict) else row.get("host_exec", {})
                attempted = bool(host_exec.get("attempted", False))
                passed = bool(host_exec.get("status_ok", False))
                if not attempted:
                    if target in host_exec_ex:
                        _append(checks, "cpu-host-exec", True, f"{target}: host exec not attempted (excepted)", level="warn")
                        continue
                    _append(checks, "cpu-host-exec", True, f"{target}: host exec not attempted", level="warn")
                    continue
                if not passed and target not in host_exec_ex:
                    _append(checks, "cpu-host-exec", False, f"{target}: host exec failed")
                    ok = False
                else:
                    _append(checks, "cpu-host-exec", True, f"{target}: host exec passed")

    return checks, ok


def evaluate_microcontroller_section(
    policy: Dict[str, Any],
    micro_payload: Dict[str, Any],
    system: str,
) -> Tuple[List[Dict[str, Any]], bool, Dict[str, int]]:
    cfg = policy.get("microcontroller", {})
    if not isinstance(cfg, dict):
        return (
            [{"name": "microcontroller-policy", "status": "fail", "message": "missing microcontroller policy block"}],
            False,
            {
                "micro_build_checked": 0,
                "micro_build_passed": 0,
                "micro_interpret_checked": 0,
                "micro_interpret_passed": 0,
            },
        )

    embedded_targets = [spec.triple for spec in TARGET_SPECS if spec.tier == "embedded"]
    required_build_cfg = cfg.get("required_build_targets")
    required_interpret_cfg = cfg.get("required_interpret_targets")

    # Backward compatibility: if canonical build list is missing, reuse legacy interpret list.
    if required_build_cfg is None:
        required_build_targets = _expand_targets(required_interpret_cfg, embedded_targets, system)
    else:
        required_build_targets = _expand_targets(required_build_cfg, embedded_targets, system)

    if required_interpret_cfg is None:
        required_interpret_targets: List[str] = []
    else:
        required_interpret_targets = _expand_targets(required_interpret_cfg, embedded_targets, system)

    build_ex = _exception_targets(cfg.get("exceptions", {}), "build_failures", system)
    interpret_ex = _exception_targets(cfg.get("exceptions", {}), "interpret_failures", system)
    rows = _load_micro_rows(micro_payload)
    checks: List[Dict[str, Any]] = []
    ok = True
    build_checked = 0
    build_passed = 0
    interpret_checked = 0
    interpret_passed = 0

    for target in required_build_targets:
        row = rows.get(target)
        if not row:
            if target in build_ex:
                _append(checks, f"micro-build[{target}]", True, "target missing but excepted", level="warn")
                continue
            _append(checks, f"micro-build[{target}]", False, "target missing in microcontroller gate payload")
            ok = False
            continue

        spec = get_target_spec(target)
        mode = str(row.get("mode", "")) or (spec.mode if spec is not None else "")
        if mode == "interpret_only":
            _append(
                checks,
                f"micro-build[{target}]",
                True,
                "target is interpret_only; build check is intentionally non-applicable",
                level="warn",
            )
            continue

        build_checked += 1
        attempted = bool(row.get("build_attempted", False))
        status_ok = bool(row.get("build_ok", False))
        if not attempted or not status_ok:
            message = f"build did not pass (attempted={attempted}, pass={status_ok})"
            if target in build_ex:
                _append(checks, f"micro-build[{target}]", True, f"{message} (excepted)", level="warn")
            else:
                _append(checks, f"micro-build[{target}]", False, message)
                ok = False
            continue

        build_passed += 1
        _append(checks, f"micro-build[{target}]", True, "build passed")

    for target in required_interpret_targets:
        row = rows.get(target)
        if not row:
            if target in interpret_ex:
                _append(checks, f"micro-interpret[{target}]", True, "target missing but excepted", level="warn")
                continue
            _append(checks, f"micro-interpret[{target}]", False, "target missing in microcontroller gate payload")
            ok = False
            continue

        spec = get_target_spec(target)
        mode = str(row.get("mode", "")) or (spec.mode if spec is not None else "")
        if mode != "interpret_only":
            _append(
                checks,
                f"micro-interpret[{target}]",
                True,
                f"target mode={mode or 'unknown'}; interpret check skipped",
                level="warn",
            )
            continue

        interpret = row.get("interpret", {})
        if not isinstance(interpret, dict):
            interpret = {}
        interpret_checked += 1
        attempted = bool(interpret.get("attempted", False))
        status_ok = bool(interpret.get("status_ok", False))

        if not attempted:
            message = "interpret smoke not attempted"
            if target in interpret_ex:
                _append(checks, f"micro-interpret[{target}]", True, f"{message} (excepted)", level="warn")
            else:
                _append(checks, f"micro-interpret[{target}]", False, message)
                ok = False
            continue

        if not status_ok:
            message = "interpret smoke failed"
            if target in interpret_ex:
                _append(checks, f"micro-interpret[{target}]", True, f"{message} (excepted)", level="warn")
            else:
                _append(checks, f"micro-interpret[{target}]", False, message)
                ok = False
            continue

        interpret_passed += 1
        _append(checks, f"micro-interpret[{target}]", True, "interpret smoke passed")

    return (
        checks,
        ok,
        {
            "micro_build_checked": build_checked,
            "micro_build_passed": build_passed,
            "micro_interpret_checked": interpret_checked,
            "micro_interpret_passed": interpret_passed,
        },
    )


def evaluate_gpu_section(policy: Dict[str, Any], gpu_payload: Dict[str, Any], system: str) -> Tuple[List[Dict[str, Any]], bool]:
    cfg = policy.get("gpu", {})
    if not isinstance(cfg, dict):
        return [{"name": "gpu-policy", "status": "fail", "message": "missing gpu policy block"}], False

    rows = _load_gpu_rows(gpu_payload)
    checks: List[Dict[str, Any]] = []
    required, optional = _resolve_gpu_policy_targets(cfg.get("required_available_targets"), system)
    if not rows and (required or optional):
        _append(checks, "gpu-backends", False, "no GPU rows loaded")
        return checks, False

    ok = True
    if not required and not optional:
        _append(checks, "gpu-backends", True, "no gpu policy constraints configured", level="warn")
    exceptions = _exception_targets(cfg.get("exceptions", {}), "available", system)

    for backend in sorted(required):
        row = rows.get(backend, {})
        if not row:
            if backend in exceptions:
                _append(checks, f"gpu[{backend}]", True, "backend row missing but excepted", level="warn")
            else:
                _append(checks, f"gpu[{backend}]", False, "backend missing in payload")
                ok = False
            continue

        available = bool(row.get("available", False))
        reason = str(row.get("reason", ""))
        mode = str(row.get("mode", ""))
        if not available:
            if mode == "planning_only":
                _append(checks, f"gpu[{backend}]", True, "planning-only backend is intentionally unavailable for strict runtime readiness", level="warn")
                continue
            if "os_not_supported" in reason and "planning_only" != mode:
                if backend in exceptions:
                    _append(checks, f"gpu[{backend}]", True, f"{reason} (excepted)", level="warn")
                else:
                    _append(checks, f"gpu[{backend}]", False, reason or "backend unavailable on host")
                    ok = False
                continue
            if backend in exceptions:
                _append(checks, f"gpu[{backend}]", True, f"{reason or 'backend unavailable'} (excepted)", level="warn")
            else:
                _append(checks, f"gpu[{backend}]", False, f"backend unavailable: {reason or 'toolchain missing'}")
                ok = False
            continue

        _append(checks, f"gpu[{backend}]", True, "backend available")

    for backend in sorted(optional):
        row = rows.get(backend, {})
        if not row:
            _append(checks, f"gpu[{backend}]", True, "optional backend row missing", level="warn")
            continue

        if not bool(row.get("available", False)):
            _append(
                checks,
                f"gpu[{backend}]",
                True,
                "optional backend unavailable; not failing readiness gate",
                level="warn",
            )
            continue

        _append(checks, f"gpu[{backend}]", True, "optional backend available")

    unknown_row_backends = sorted(set(rows) - required - optional)
    if unknown_row_backends:
        _append(checks, "gpu-backends", True, f"detected unconfigured backends: {', '.join(unknown_row_backends)}", level="warn")

    return checks, ok


def evaluate_performance_section(policy: Dict[str, Any], perf_payload: Dict[str, Any], system: str) -> Tuple[List[Dict[str, Any]], bool]:
    cfg = policy.get("performance", {})
    if not isinstance(cfg, dict):
        return [{"name": "performance-policy", "status": "fail", "message": "missing performance policy block"}], False

    checks: List[Dict[str, Any]] = []
    ok = True
    min_count = int(cfg.get("min_sample_count", 1) or 1)

    rows = _load_perf_rows(perf_payload)
    if not rows:
        _append(checks, "performance-payload", False, "missing performance target rows")
        return checks, False

    interpret_targets = _expand_targets(cfg.get("required_interpret_targets"), rows.keys(), system)
    required_targets = interpret_targets
    if cfg.get("required_targets") == "host":
        host_arch = _normalize_arch(platform.machine())
        host_rows = _find_host_targets(
            {name: {"target": name, "mode": "aot", **row} for name, row in rows.items()},
            system,
            host_arch,
        )
        if not host_rows:
            _append(checks, "performance-host", False, f"no host performance row for arch={host_arch}, os={system}")
            ok = False
        else:
            host_ok = True
            for row in host_rows:
                target = str(row.get("target", ""))
                perf_row = row
                interpret = _interpret_record(perf_row)
                if not bool(interpret.get("attempted", False)):
                    host_ok = False
                    _append(checks, "performance-host", False, f"{target}: host performance not attempted")
                    ok = False
                    continue
                sample_count = int(interpret.get("sample_count", 0) or 0)
                if sample_count < min_count:
                    host_ok = False
                    _append(checks, "performance-host", False, f"{target}: sample_count={sample_count} < required {min_count}")
                    ok = False
                    continue
                if not bool(interpret.get("status_ok", False)):
                    host_ok = False
                    _append(checks, "performance-host", False, f"{target}: host performance failed")
                    ok = False
            if host_ok:
                _append(checks, "performance-host", True, "host performance criteria met")

    for target in required_targets:
        row = rows.get(target)
        if not row:
            _append(checks, f"performance-interpret[{target}]", False, "target missing in performance payload")
            ok = False
            continue
        interpret = _interpret_record(row)
        if not bool(interpret.get("attempted", False)):
            _append(checks, f"performance-interpret[{target}]", False, "interpret execution not attempted")
            ok = False
            continue
        sample_count = int(interpret.get("sample_count", 0) or 0)
        if sample_count < min_count:
            _append(checks, f"performance-interpret[{target}]", False, f"sample_count={sample_count} < required {min_count}")
            ok = False
            continue
        if not bool(interpret.get("status_ok", False)):
            _append(checks, f"performance-interpret[{target}]", False, "interpret performance execution failed")
            ok = False
            continue
        _append(checks, f"performance-interpret[{target}]", True, "interpret performance passed")

    return checks, ok


def main() -> int:
    parser = argparse.ArgumentParser(description="Enforce end-to-end portability policy for CPU/GPU/microcontroller checks.")
    parser.add_argument("--policy", default=str(REPO_ROOT / "doc/platform_support_policy.json"))
    parser.add_argument("--platform-matrix", default=_default_for_host("platform_support_platform_matrix"))
    parser.add_argument("--advanced-capability", default=_default_for_host("platform_support_advanced_capability"))
    parser.add_argument("--cross-perf", default=_default_for_host("platform_support_cross_platform_perf"))
    parser.add_argument("--microcontroller", default=_default_for_host("platform_support_microcontroller_gate"))
    parser.add_argument("--gpu-smoke", default=_default_for_host("platform_support_gpu_smoke"))
    parser.add_argument(
        "--require-gpu",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Set to false to skip GPU availability checks in hosted matrix runs.",
    )
    parser.add_argument("--report-out", default=_default_for_host("platform_support_platform_support_gate"))
    args = parser.parse_args()

    policy = _load_policy(REPO_ROOT / args.policy)
    report: Dict[str, Any] = {
        "policy_file": str((REPO_ROOT / args.policy).resolve()),
        "system": platform.system(),
        "checks": [],
    }

    platform_matrix_path = REPO_ROOT / args.platform_matrix
    advanced_path = REPO_ROOT / args.advanced_capability
    perf_path = REPO_ROOT / args.cross_perf
    micro_path = REPO_ROOT / args.microcontroller
    gpu_path = REPO_ROOT / args.gpu_smoke

    all_ok = True

    platform_payload = _load_json(platform_matrix_path)
    advanced_payload = _load_json(advanced_path)
    perf_payload = _load_json(perf_path)
    micro_payload = _load_json(micro_path)
    gpu_payload = _load_json(gpu_path) if args.require_gpu else {}

    report["source_files"] = {
        "platform_matrix": str(platform_payload.get("meta", {}).get("source", str(platform_matrix_path)))
        if isinstance(platform_payload, dict)
        else str(platform_matrix_path),
        "advanced_capability": str(advanced_path),
        "cross_perf": str(perf_path),
        "microcontroller": str(micro_path),
        "gpu_smoke": str(gpu_path),
    }

    host_system = _normalize_os(platform.system())

    cpu_checks, cpu_ok = evaluate_cpu_section(policy, platform_payload, host_system, advanced_payload)
    if "microcontroller" in policy:
        micro_checks, micro_ok, micro_summary = evaluate_microcontroller_section(policy, micro_payload, host_system)
    else:
        micro_checks, micro_ok, micro_summary = (
            [],
            True,
            {
                "micro_build_checked": 0,
                "micro_build_passed": 0,
                "micro_interpret_checked": 0,
                "micro_interpret_passed": 0,
            },
        )
    if args.require_gpu:
        gpu_checks, gpu_ok = evaluate_gpu_section(policy, gpu_payload, host_system)
    else:
        gpu_checks = []
        _append(gpu_checks, "gpu-backends", True, "gpu availability checks skipped for hosted matrix run", level="warn")
        gpu_ok = True
    perf_checks, perf_ok = evaluate_performance_section(policy, perf_payload, host_system)

    report["checks"].extend(cpu_checks)
    report["checks"].extend(micro_checks)
    report["checks"].extend(gpu_checks)
    report["checks"].extend(perf_checks)
    all_ok = cpu_ok and micro_ok and gpu_ok and perf_ok

    report["result"] = {
        "pass": bool(all_ok),
        "system": host_system,
        **micro_summary,
    }
    report_path = REPO_ROOT / args.report_out
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    failed = [item["name"] + ": " + str(item["message"]) for item in report["checks"] if item.get("status") == "fail"]
    warnings = [item["name"] + ": " + str(item["message"]) for item in report["checks"] if item.get("level") == "warn"]

    for item in warnings:
        print(f"platform_support_gate WARN: {item}")
    if failed:
        for item in failed:
            print(f"platform_support_gate FAIL: {item}")
        print(f"platform_support_gate report: {report_path}")
        return 1

    print("platform_support_gate PASS")
    print(f"platform_support_gate report: {report_path}")
    print(f"checks: {len(report['checks'])}, warnings: {len(warnings)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
