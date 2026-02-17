from dataclasses import dataclass
from pathlib import Path
from typing import List


@dataclass(frozen=True)
class BenchmarkDefinition:
    name: str
    source: str
    baseline_c: str
    min_own_vs_c: float
    max_own_vs_blas: float
    max_auto_vs_best: float


def phase8_benchmarks(root_dir: Path) -> List[BenchmarkDefinition]:
    _ = root_dir
    return [
        BenchmarkDefinition(
            name="matmul_core_f64",
            source="matmul_core_f64.k",
            baseline_c="c_baselines/matmul_core_f64.c",
            min_own_vs_c=0.005,
            max_own_vs_blas=2.0,
            max_auto_vs_best=1.25,
        ),
        BenchmarkDefinition(
            name="matmul_epilogue_f64",
            source="matmul_epilogue_f64.k",
            baseline_c="c_baselines/matmul_epilogue_f64.c",
            min_own_vs_c=0.005,
            max_own_vs_blas=2.0,
            max_auto_vs_best=1.25,
        ),
        BenchmarkDefinition(
            name="matmul_core_f64_256",
            source="matmul_core_f64_256.k",
            baseline_c="c_baselines/matmul_core_f64_256.c",
            min_own_vs_c=0.005,
            max_own_vs_blas=2.0,
            max_auto_vs_best=1.25,
        ),
        BenchmarkDefinition(
            name="matmul_core_f64_512",
            source="matmul_core_f64_512.k",
            baseline_c="c_baselines/matmul_core_f64_512.c",
            min_own_vs_c=0.005,
            max_own_vs_blas=2.0,
            max_auto_vs_best=1.25,
        ),
    ]
