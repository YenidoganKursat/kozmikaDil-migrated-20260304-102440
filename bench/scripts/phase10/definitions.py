from dataclasses import dataclass
from typing import List


@dataclass(frozen=True)
class DispatchVariant:
    name: str
    arch: str
    features: str


def dispatch_variants() -> List[DispatchVariant]:
    return [
        DispatchVariant(name="host_default", arch="", features=""),
        DispatchVariant(name="x86_avx2", arch="x86_64", features="sse2,avx2"),
        DispatchVariant(name="x86_avx512", arch="x86_64", features="sse2,avx2,avx512f"),
        DispatchVariant(name="arm_neon", arch="aarch64", features="neon"),
        DispatchVariant(name="arm_sve2", arch="aarch64", features="neon,sve2"),
    ]


def phase10_targets() -> str:
    return "x86_64-linux-gnu,aarch64-linux-gnu,riscv64-linux-gnu"
