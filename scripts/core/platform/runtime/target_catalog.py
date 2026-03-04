#!/usr/bin/env python3
"""Target catalog for platform_support portability orchestration.

This module centralizes target triples, support tiers, and presets so build
orchestration can scale without hard-coding small target lists in multiple
scripts.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Iterable, List, Sequence


@dataclass(frozen=True)
class TargetSpec:
    triple: str
    family: str
    os_class: str
    tier: str
    mode: str
    notes: str


# tier:
# - stable: currently expected to be buildable in userland with standard toolchains
# - experimental: supported in principle, but depends on cross toolchain/sysroot presence
# - embedded: bare-metal/MCU targets; requires dedicated cross toolchains in CI/dev hosts
TARGET_SPECS: Sequence[TargetSpec] = (
    TargetSpec("x86_64-linux-gnu", "x86_64", "linux", "stable", "aot", "Desktop/server Linux"),
    TargetSpec("aarch64-linux-gnu", "arm64", "linux", "stable", "aot", "ARM64 Linux (Raspberry Pi 64-bit, ARM servers)"),
    TargetSpec("i686-linux-gnu", "x86", "linux", "experimental", "aot", "Legacy 32-bit x86 Linux"),
    TargetSpec("riscv64-linux-gnu", "riscv64", "linux", "experimental", "aot", "RISC-V Linux userland"),
    TargetSpec("armv7-linux-gnueabihf", "armv7", "linux", "experimental", "aot", "ARMv7 Linux (legacy Raspberry Pi OS 32-bit)"),
    TargetSpec("riscv32-linux-gnu", "riscv32", "linux", "experimental", "aot", "RISC-V 32-bit Linux userland"),
    TargetSpec("ppc64le-linux-gnu", "ppc64le", "linux", "experimental", "aot", "PowerPC LE Linux"),
    TargetSpec("s390x-linux-gnu", "s390x", "linux", "experimental", "aot", "s390x Linux"),
    TargetSpec("loongarch64-linux-gnu", "loongarch64", "linux", "experimental", "aot", "LoongArch64 Linux"),
    TargetSpec("mips64el-linux-gnuabi64", "mips64", "linux", "experimental", "aot", "MIPS64EL Linux"),
    TargetSpec("mipsel-linux-gnu", "mips", "linux", "experimental", "aot", "MIPS32EL Linux"),
    TargetSpec("x86_64-apple-darwin", "x86_64", "darwin", "stable", "aot", "Intel macOS"),
    TargetSpec("arm64-apple-darwin", "arm64", "darwin", "stable", "aot", "Apple Silicon macOS"),
    TargetSpec("x86_64-w64-mingw32", "x86_64", "windows", "experimental", "aot", "Windows x64 via MinGW cross toolchain"),
    TargetSpec("aarch64-w64-mingw32", "arm64", "windows", "experimental", "aot", "Windows ARM64 via MinGW cross toolchain"),
    TargetSpec("x86_64-unknown-freebsd", "x86_64", "freebsd", "experimental", "aot", "FreeBSD x64"),
    TargetSpec("aarch64-unknown-freebsd", "arm64", "freebsd", "experimental", "aot", "FreeBSD ARM64"),
    TargetSpec("aarch64-linux-android", "arm64", "android", "experimental", "aot", "Android ARM64 (NDK toolchain/sysroot)"),
    TargetSpec("x86_64-linux-android", "x86_64", "android", "experimental", "aot", "Android x64 (emulator/dev host)"),
    TargetSpec("armv7a-linux-androideabi", "armv7", "android", "experimental", "aot", "Android ARMv7"),
    TargetSpec("wasm32-wasi", "wasm32", "web", "experimental", "aot", "WASI userland WebAssembly"),
    TargetSpec("wasm32-unknown-emscripten", "wasm32", "web", "experimental", "aot", "Browser/WebAssembly via Emscripten"),
    TargetSpec("arm-none-eabi", "arm-mcu", "embedded", "embedded", "aot", "Cortex-M bare-metal (requires arm-none-eabi toolchain)"),
    TargetSpec("riscv64-unknown-elf", "riscv-mcu", "embedded", "embedded", "aot", "RISC-V bare-metal (requires riscv64-unknown-elf toolchain)"),
    TargetSpec("avr-none-elf", "avr", "embedded", "embedded", "aot", "Arduino AVR bare-metal (requires avr toolchain)"),
    TargetSpec("xtensa-esp32-elf", "xtensa", "embedded", "embedded", "aot", "ESP32 bare-metal/RTOS (requires xtensa-esp32-elf toolchain)"),
)


def _by_triple() -> Dict[str, TargetSpec]:
    return {item.triple: item for item in TARGET_SPECS}


def get_target_spec(triple: str) -> TargetSpec | None:
    return _by_triple().get(triple)


def preset_targets(name: str) -> List[str]:
    presets = {
    "core": ["x86_64-linux-gnu", "aarch64-linux-gnu", "riscv64-linux-gnu"],
    "linux": [
        "x86_64-linux-gnu",
        "aarch64-linux-gnu",
        "riscv64-linux-gnu",
        "riscv32-linux-gnu",
        "i686-linux-gnu",
        "armv7-linux-gnueabihf",
        "ppc64le-linux-gnu",
        "s390x-linux-gnu",
        "mips64el-linux-gnuabi64",
        "mipsel-linux-gnu",
        "loongarch64-linux-gnu",
    ],
        "desktop": ["x86_64-linux-gnu", "aarch64-linux-gnu", "x86_64-apple-darwin", "arm64-apple-darwin"],
        "mobile": ["aarch64-linux-android", "x86_64-linux-android", "armv7a-linux-androideabi"],
        "windows": ["x86_64-w64-mingw32", "aarch64-w64-mingw32"],
        "web": ["wasm32-wasi", "wasm32-unknown-emscripten"],
        "freebsd": ["x86_64-unknown-freebsd", "aarch64-unknown-freebsd"],
        "embedded": ["arm-none-eabi", "riscv64-unknown-elf", "avr-none-elf", "xtensa-esp32-elf"],
        "market": [item.triple for item in TARGET_SPECS],
    }
    if name not in presets:
        raise ValueError(f"unknown preset: {name}")
    return list(presets[name])


def list_presets() -> List[str]:
    return ["core", "linux", "desktop", "mobile", "windows", "web", "freebsd", "embedded", "market"]


def resolve_targets(
    explicit_targets: Iterable[str] | None,
    preset: str | None,
    include_experimental: bool,
    include_embedded: bool,
) -> List[str]:
    explicit_mode = bool(explicit_targets)
    if explicit_targets:
        ordered = [item.strip() for item in explicit_targets if item and item.strip()]
    elif preset:
        ordered = preset_targets(preset)
    else:
        ordered = preset_targets("core")

    seen = set()
    out: List[str] = []
    catalog = _by_triple()
    for triple in ordered:
        if triple in seen:
            continue
        seen.add(triple)
        spec = catalog.get(triple)
        if spec is None:
            out.append(triple)
            continue
        # Explicit target lists are considered intentional and bypass preset-tier filters.
        if not explicit_mode:
            if spec.tier == "embedded" and not include_embedded:
                continue
            if spec.tier == "experimental" and not include_experimental:
                continue
        out.append(triple)
    return out
