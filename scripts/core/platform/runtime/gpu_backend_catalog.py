#!/usr/bin/env python3
"""GPU backend catalog and host capability probing for platform_support."""

from __future__ import annotations

import platform
import shutil
import subprocess
from dataclasses import dataclass
from typing import Dict, List, Sequence


@dataclass(frozen=True)
class GpuBackendSpec:
    name: str
    api: str
    tier: str
    mode: str
    notes: str
    probe_commands: Sequence[Sequence[str]]
    os_allow: Sequence[str]


GPU_BACKENDS: Sequence[GpuBackendSpec] = (
    GpuBackendSpec(
        name="cuda",
        api="CUDA",
        tier="experimental",
        mode="native_accel",
        notes="NVIDIA CUDA backend (requires CUDA toolkit + driver)",
        probe_commands=(("nvcc", "--version"), ("nvidia-smi", "--query-gpu=name", "--format=csv,noheader")),
        os_allow=("Linux", "Windows"),
    ),
    GpuBackendSpec(
        name="rocm_hip",
        api="HIP/ROCm",
        tier="experimental",
        mode="native_accel",
        notes="AMD ROCm HIP backend",
        probe_commands=(("hipcc", "--version"), ("rocminfo",), ("rocm-smi", "--showproductname")),
        os_allow=("Linux",),
    ),
    GpuBackendSpec(
        name="oneapi_sycl",
        api="SYCL",
        tier="experimental",
        mode="native_accel",
        notes="Intel/oneAPI SYCL backend",
        probe_commands=(("dpcpp", "--version"), ("icpx", "--version")),
        os_allow=("Linux", "Windows"),
    ),
    GpuBackendSpec(
        name="opencl",
        api="OpenCL",
        tier="experimental",
        mode="native_accel",
        notes="Portable OpenCL compute backend",
        probe_commands=(("clinfo",),),
        os_allow=("Linux", "Darwin", "Windows"),
    ),
    GpuBackendSpec(
        name="vulkan_compute",
        api="Vulkan",
        tier="experimental",
        mode="native_accel",
        notes="Vulkan compute backend",
        probe_commands=(("vulkaninfo", "--summary"),),
        os_allow=("Linux", "Windows"),
    ),
    GpuBackendSpec(
        name="metal",
        api="Metal",
        tier="stable",
        mode="native_accel",
        notes="Apple Metal backend",
        probe_commands=(("xcrun", "-f", "metal"),),
        os_allow=("Darwin",),
    ),
    GpuBackendSpec(
        name="webgpu",
        api="WebGPU",
        tier="planning",
        mode="planning_only",
        notes="Browser/WebGPU backend planning target",
        probe_commands=(),
        os_allow=("Linux", "Darwin", "Windows"),
    ),
)

# Accepted aliases for user/CLI ergonomics; values map to canonical backend ids.
BACKEND_ALIASES: Dict[str, str] = {
    "nvidia": "cuda",
    "rocm": "rocm_hip",
    "hip": "rocm_hip",
    "sycl": "oneapi_sycl",
    "oneapi": "oneapi_sycl",
    "intel": "oneapi_sycl",
    "cl": "opencl",
    "vulkan": "vulkan_compute",
    "vk": "vulkan_compute",
}


def known_backend_names() -> List[str]:
    return [item.name for item in GPU_BACKENDS]


def canonicalize_backend_name(name: str) -> str:
    token = name.strip().lower()
    if not token:
        return ""
    return BACKEND_ALIASES.get(token, token)


def normalize_backend_list(values: Sequence[str]) -> List[str]:
    out: List[str] = []
    seen = set()
    for item in values:
        canonical = canonicalize_backend_name(item)
        if not canonical or canonical in seen:
            continue
        seen.add(canonical)
        out.append(canonical)
    return out


def _which_first_token(command: Sequence[str]) -> bool:
    if not command:
        return False
    return shutil.which(command[0]) is not None


def _probe_command(command: Sequence[str]) -> bool:
    if not command:
        return False
    if not _which_first_token(command):
        return False
    try:
        proc = subprocess.run(
            list(command),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
            timeout=4,
        )
    except Exception:  # noqa: BLE001
        return False
    return proc.returncode == 0


def detect_gpu_backend(spec: GpuBackendSpec) -> Dict[str, object]:
    host_os = platform.system()
    allowed = host_os in spec.os_allow
    if not allowed:
        return {
            "name": spec.name,
            "api": spec.api,
            "tier": spec.tier,
            "mode": spec.mode,
            "available": False,
            "reason": f"os_not_supported:{host_os}",
            "notes": spec.notes,
        }

    if spec.mode == "planning_only":
        return {
            "name": spec.name,
            "api": spec.api,
            "tier": spec.tier,
            "mode": spec.mode,
            "available": False,
            "reason": "planning_only",
            "notes": spec.notes,
        }

    found_binary = any(_which_first_token(cmd) for cmd in spec.probe_commands)
    if not found_binary:
        return {
            "name": spec.name,
            "api": spec.api,
            "tier": spec.tier,
            "mode": spec.mode,
            "available": False,
            "reason": "tool_not_found",
            "notes": spec.notes,
        }

    for cmd in spec.probe_commands:
        if _probe_command(cmd):
            return {
                "name": spec.name,
                "api": spec.api,
                "tier": spec.tier,
                "mode": spec.mode,
                "available": True,
                "reason": f"probe_ok:{' '.join(cmd)}",
                "notes": spec.notes,
            }

    return {
        "name": spec.name,
        "api": spec.api,
        "tier": spec.tier,
        "mode": spec.mode,
        "available": False,
        "reason": "probe_failed",
        "notes": spec.notes,
    }


def resolve_gpu_backends(include_experimental: bool, include_planning: bool) -> List[GpuBackendSpec]:
    out: List[GpuBackendSpec] = []
    for spec in GPU_BACKENDS:
        if spec.tier == "experimental" and not include_experimental:
            continue
        if spec.tier == "planning" and not include_planning:
            continue
        out.append(spec)
    return out
