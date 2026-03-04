#!/usr/bin/env python3
"""Prepare and verify GPU backend infrastructure for strict smoke checks."""

from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from gpu_backend_catalog import (  # noqa: E402
    GpuBackendSpec,
    GPU_BACKENDS,
    canonicalize_backend_name,
    detect_gpu_backend,
    known_backend_names,
    normalize_backend_list,
)


def _default_json_path() -> Path:
    return Path("bench/results/platform_support_gpu_infra.json")


LINUX_APT_PACKAGES: Dict[str, List[str]] = {
    "cuda": ["nvidia-cuda-toolkit"],
    "opencl": ["clinfo", "ocl-icd-opencl-dev", "opencl-headers", "opencl-c-headers"],
    "vulkan_compute": ["vulkan-tools", "vulkan-validationlayers", "vulkan-headers"],
}

LINUX_DISTRIBUTION_APT_PACKAGES: Dict[str, List[str]] = {
    "oneapi_sycl": ["intel-basekit", "intel-oneapi-dpcpp-cpp"],
    "rocm_hip": ["rocm-core", "hip-runtime-amd", "rocm-libs"],
}

WINDOWS_WINGET_PACKAGES: Dict[str, List[str]] = {
    "cuda": ["--id", "NVIDIA.CUDA", "--source", "winget"],
    "vulkan_compute": ["--id", "KhronosGroup.VulkanSDK", "--source", "winget"],
}

DARWIN_BREW_PACKAGES: Dict[str, List[str]] = {
    "opencl": ["clinfo"],
    "vulkan_compute": ["vulkan-headers", "vulkan-tools"],
}


MANUAL_HINTS: Dict[str, Dict[str, str]] = {
    "cuda": {
        "Windows": "Install NVIDIA CUDA toolkit/driver (for example via official installer or winget id NVIDIA.CUDA).",
        "Darwin": "CUDA is not supported on macOS.",
    },
    "rocm_hip": {
        "Linux": "Install ROCm (hipcc/rocminfo/rocm-smi) from AMD official ROCm docs for your distro.",
        "Windows": "ROCm is not available on Windows.",
        "Darwin": "ROCm is not available on macOS.",
    },
    "oneapi_sycl": {
        "Linux": "Install Intel oneAPI DPC++/ICX toolchain (dpcpp/icpx) from Intel oneAPI setup guide.",
        "Windows": "Install Intel oneAPI DPC++/ICX toolchain and add dpcpp/icpx to PATH (manual for this environment).",
        "Darwin": "oneAPI SYCL is currently not available on macOS.",
    },
    "opencl": {
        "Windows": "Install GPU vendor OpenCL runtime and clinfo utility, then add vendor OpenCL runtime DLLs to PATH.",
        "Darwin": "OpenCL runtime is host-dependent; for command probing install clinfo (`brew install clinfo`) if not present.",
    },
    "vulkan_compute": {
        "Windows": "Install Vulkan SDK or latest driver layer runtime (LunarG/Khronos) and ensure vulkaninfo is on PATH.",
        "Darwin": "Install Vulkan SDK or moltenvk path and runtime components; if available, install `vulkan-tools` via brew.",
    },
    "webgpu": {
        "all": "WebGPU target is currently planning-only; native provision is not implemented yet.",
    },
}

MANUAL_HINT_NOT_SUPPORTED: Dict[str, str] = {
    "metal": "Metal is macOS-only; use an Xcode-enabled Apple GPU runner and verify `xcrun -f metal`.",
    "rocm_hip": "ROCm is Linux-only in current tooling.",
    "oneapi_sycl": "oneAPI SYCL is supported on Linux and Windows only in current catalog.",
    "webgpu": "WebGPU is a planning target and has no native probe/provision path yet.",
}


def parse_backends(raw: str) -> list[str]:
    raw = raw.strip().lower()
    if raw in {"", "all"}:
        return known_backend_names()
    selected = normalize_backend_list([item.strip() for item in raw.split(",") if item.strip()])
    known = set(known_backend_names())
    unknown = [item for item in selected if canonicalize_backend_name(item) not in known]
    if unknown:
        raise SystemExit(f"unknown backend(s): {', '.join(unknown)}")
    return selected


def _find_spec(name: str) -> GpuBackendSpec | None:
    for item in GPU_BACKENDS:
        if item.name == name:
            return item
    return None


def _run_cmd(command: Iterable[str]) -> tuple[int, str, str]:
    proc = subprocess.run(
        list(command),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return proc.returncode, proc.stdout, proc.stderr


def _sudo_available() -> bool:
    return shutil.which("sudo") is not None


def _apt_get_available() -> bool:
    return shutil.which("apt-get") is not None


def _apt_package_map(backend: str) -> List[str] | None:
    packages = LINUX_APT_PACKAGES.get(backend)
    if packages:
        return packages
    packages = LINUX_DISTRIBUTION_APT_PACKAGES.get(backend)
    if packages:
        return packages
    return None


def _brew_available() -> bool:
    return shutil.which("brew") is not None


def _winget_available() -> bool:
    return shutil.which("winget") is not None


def _windows_install_command(spec: GpuBackendSpec) -> Tuple[str, List[str]] | None:
    args = WINDOWS_WINGET_PACKAGES.get(spec.name)
    if not args:
        return None
    return "winget", ["winget", "install", "--accept-package-agreements", "--accept-source-agreements", *args]


def _darwin_install_command(spec: GpuBackendSpec) -> Tuple[str, List[str]] | None:
    packages = DARWIN_BREW_PACKAGES.get(spec.name)
    if not packages:
        return None
    return "brew", ["brew", "install", *packages]


def _install_linux_packages(packages: List[str], backend: str) -> tuple[bool, str]:
    if not packages:
        return False, f"{backend}: no apt packages configured"
    if not _apt_get_available():
        return False, f"{backend}: apt-get unavailable on host"
    rc, out, _ = _run_cmd(["apt-get", "-o", "Acquire::Retries=3", "update"])
    if rc != 0:
        return False, f"{backend}: apt-get update failed"

    install_cmd = ["apt-get", "-o", "Acquire::Retries=3", "install", "-y", *packages]
    if _sudo_available():
        install_cmd.insert(0, "sudo")
    rc, out, _ = _run_cmd(install_cmd)
    if rc != 0:
        return False, f"{backend}: apt install failed"
    return True, f"{backend}: apt packages attempted ({', '.join(packages)})"


def _install_linux_distribution_packages(backend: str, packages: List[str]) -> tuple[bool, str]:
    if not packages:
        return False, f"{backend}: no distribution packages configured"
    return _install_linux_packages(packages, backend)


def _install_windows_packages(spec: GpuBackendSpec) -> tuple[bool, bool, str]:
    manager, command = _windows_install_command(spec)
    if manager is None:
        return (
            False,
            False,
            MANUAL_HINTS.get(spec.name, {}).get(
                "Windows",
                MANUAL_HINT_NOT_SUPPORTED.get(spec.name, f"{spec.name}: no Windows auto-provision mapping"),
            ),
        )
    if manager == "winget":
        if not _winget_available():
            return False, False, f"{spec.name}: winget not available"
        rc, out, _ = _run_cmd(command)
        if rc != 0:
            return True, False, f"{spec.name}: winget install failed ({out.strip() or 'no output'})"
        return True, True, f"{spec.name}: winget attempted ({', '.join(command[1:])})"
    return False, False, f"{spec.name}: unsupported Windows package manager path"


def _install_darwin_packages(spec: GpuBackendSpec) -> tuple[bool, bool, str]:
    install_command = _darwin_install_command(spec)
    if install_command is None:
        return (
            False,
            False,
            MANUAL_HINTS.get(spec.name, {}).get(
                "Darwin",
                MANUAL_HINT_NOT_SUPPORTED.get(spec.name, f"{spec.name}: no macOS auto-provision mapping"),
            ),
        )

    manager, command = install_command
    if manager is None:
        return (
            False,
            False,
            MANUAL_HINTS.get(spec.name, {}).get(
                "Darwin",
                MANUAL_HINT_NOT_SUPPORTED.get(spec.name, f"{spec.name}: no macOS auto-provision mapping"),
            ),
        )
    if manager != "brew":
        return False, False, f"{spec.name}: unsupported macOS package manager path"
    if not _brew_available():
        return True, False, f"{spec.name}: brew unavailable"
    rc, out, _ = _run_cmd(command)
    if rc != 0:
        return True, False, f"{spec.name}: brew install failed ({out.strip() or 'no output'})"
    return True, True, f"{spec.name}: brew attempted ({', '.join(command[1:])})"


def _provision_backend(spec: GpuBackendSpec, strict: bool) -> tuple[bool, str, bool]:
    host_os = platform.system()
    if spec.mode == "planning_only":
        return False, f"{spec.name}: planning-only backend; no provisioning path", False

    attempted = False
    ok = False
    if host_os == "Linux":
        packages = _apt_package_map(spec.name)
        if not packages:
            message = MANUAL_HINTS.get(spec.name, {}).get(
                host_os,
                MANUAL_HINT_NOT_SUPPORTED.get(spec.name, f"{spec.name}: no Linux auto-provision mapping"),
            )
        else:
            attempted = True
            ok, message = _install_linux_distribution_packages(spec.name, packages)
            attempted = True
    elif host_os == "Windows":
        attempted, ok, message = _install_windows_packages(spec)
    elif host_os == "Darwin":
        attempted, ok, message = _install_darwin_packages(spec)
    else:
        return False, f"{spec.name}: auto-provisioning not implemented for {host_os}", False

    if not ok and strict:
        return attempted, message, False
    return attempted, message, ok


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare and verify GPU backend infra for strict tests.")
    parser.add_argument(
        "--backends",
        default="all",
        help="comma-separated backend names or 'all'",
    )
    parser.add_argument(
        "--provision",
        action="store_true",
        help="attempt best-effort installation of host-packaged GPU deps where supported",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit non-zero when a requested non-planning backend remains unavailable",
    )
    parser.add_argument("--json-out", default=str(_default_json_path()), help="where to write infra payload")
    args = parser.parse_args()

    selected = parse_backends(args.backends)
    host_os = platform.system()
    out: dict[str, object] = {
        "host_os": host_os,
        "requested": selected,
        "strict": bool(args.strict),
        "provision_attempted": bool(args.provision),
        "results": [],
        "summary": {
            "total": len(selected),
            "planning": 0,
            "available": 0,
            "unavailable": 0,
            "missing_count": 0,
            "provisioned": 0,
            "failed": 0,
        },
    }

    failed: list[str] = []
    for name in selected:
        spec = _find_spec(name)
        if spec is None:
            continue
        result = detect_gpu_backend(spec)
        row = {
            "name": spec.name,
            "mode": spec.mode,
            "available_before": bool(result.get("available", False)),
            "provisioned": False,
            "provisioned_command": "",
            "available_after": bool(result.get("available", False)),
            "status": "verified",
        }

        if spec.mode == "planning_only":
            row["status"] = "planning_only"
            out["summary"]["planning"] = int(out["summary"]["planning"]) + 1
            out["results"].append(row)
            continue

        if args.provision:
            attempted, message, provisioned_ok = _provision_backend(spec, args.strict)
            row["provisioned"] = bool(provisioned_ok)
            row["provisioned_command"] = message
            if attempted and provisioned_ok:
                result = detect_gpu_backend(spec)
                row["available_after"] = bool(result.get("available", False))
            if row["provisioned"]:
                out["summary"]["provisioned"] = int(out["summary"]["provisioned"]) + 1
            if attempted:
                row["status"] = "provision_attempted"

        if row["available_after"]:
            out["summary"]["available"] = int(out["summary"]["available"]) + 1
        else:
            out["summary"]["unavailable"] = int(out["summary"]["unavailable"]) + 1
            if args.strict:
                missing_hint = MANUAL_HINTS.get(spec.name, {}).get("all")
                if missing_hint is None:
                    missing_hint = MANUAL_HINTS.get(spec.name, {}).get(host_os, "")
                if not missing_hint:
                    missing_hint = str(result.get("reason", "unknown"))
                out["summary"]["failed"] = int(out["summary"]["failed"]) + 1
                failed.append(f"{name}: {missing_hint}")
                row["status"] = "missing"

        out["results"].append(row)

    out["summary"]["missing_count"] = len(failed)
    out["summary"]["strict_passed"] = len(failed) == 0

    out_path = Path(args.json_out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"[gpu-infra] wrote: {out_path}")
    for item in out["results"]:
        print(
            f"[gpu-infra] {item['name']:14} "
            f"planning={str(item['mode'] == 'planning_only'):5} "
            f"available={str(item['available_after']):5} "
            f"provisioned={str(item['provisioned']):5}"
        )

    if failed:
        print("[gpu-infra] missing backends:")
        for message in failed:
            print(f"  - {message}")
        if args.strict:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
