from dataclasses import dataclass
from pathlib import Path
from typing import List


@dataclass(frozen=True)
class BenchmarkDefinition:
    name: str
    source: str
    expected_checksum: float
    expected_plan: int
    min_speedup: float


def phase7_benchmarks(root_dir: Path) -> List[BenchmarkDefinition]:
    _ = root_dir
    return [
        BenchmarkDefinition(
            name="list_pipeline_reduce",
            source="list_pipeline_reduce.k",
            expected_checksum=275089008.0,
            expected_plan=1,
            min_speedup=1.5,
        ),
        BenchmarkDefinition(
            name="hetero_pipeline_reduce",
            source="hetero_pipeline_reduce.k",
            expected_checksum=43193100.0,
            expected_plan=5,
            min_speedup=1.8,
        ),
        BenchmarkDefinition(
            name="matrix_pipeline_reduce",
            source="matrix_pipeline_reduce.k",
            expected_checksum=15681852.0,
            expected_plan=1,
            min_speedup=1.2,
        ),
    ]
