from dataclasses import dataclass
from pathlib import Path
from typing import List


@dataclass(frozen=True)
class BenchmarkDefinition:
    name: str
    source: str
    group: str
    mode: str
    ops_per_run: int
    expected_total: float
    expected_plan: int
    expected_materialize_min: int
    expected_cache_bytes_min: int


def phase6_benchmarks(root_dir: Path) -> List[BenchmarkDefinition]:
    _ = root_dir  # path ownership is validated by the caller.
    return [
        BenchmarkDefinition(
            name="packed_reduce_sum_steady",
            source="packed_reduce_sum_steady.k",
            group="packed",
            mode="steady",
            ops_per_run=81,
            expected_total=583195140000.0,
            expected_plan=1,
            expected_materialize_min=0,
            expected_cache_bytes_min=0,
        ),
        BenchmarkDefinition(
            name="hetero_promote_reduce_first",
            source="hetero_promote_reduce_first.k",
            group="promote",
            mode="first",
            ops_per_run=1,
            expected_total=7199970000.0,
            expected_plan=3,
            expected_materialize_min=1,
            expected_cache_bytes_min=1,
        ),
        BenchmarkDefinition(
            name="hetero_promote_reduce_steady",
            source="hetero_promote_reduce_steady.k",
            group="promote",
            mode="steady",
            ops_per_run=81,
            expected_total=583197570000.0,
            expected_plan=3,
            expected_materialize_min=1,
            expected_cache_bytes_min=1,
        ),
        BenchmarkDefinition(
            name="hetero_chunk_reduce_first",
            source="hetero_chunk_reduce_first.k",
            group="chunk",
            mode="first",
            ops_per_run=1,
            expected_total=2879916000.0,
            expected_plan=4,
            expected_materialize_min=1,
            expected_cache_bytes_min=1,
        ),
        BenchmarkDefinition(
            name="hetero_chunk_reduce_steady",
            source="hetero_chunk_reduce_steady.k",
            group="chunk",
            mode="steady",
            ops_per_run=81,
            expected_total=233273196000.0,
            expected_plan=4,
            expected_materialize_min=1,
            expected_cache_bytes_min=1,
        ),
        BenchmarkDefinition(
            name="hetero_gather_reduce_first",
            source="hetero_gather_reduce_first.k",
            group="gather",
            mode="first",
            ops_per_run=1,
            expected_total=5759928000.0,
            expected_plan=5,
            expected_materialize_min=1,
            expected_cache_bytes_min=1,
        ),
        BenchmarkDefinition(
            name="hetero_gather_reduce_steady",
            source="hetero_gather_reduce_steady.k",
            group="gather",
            mode="steady",
            ops_per_run=81,
            expected_total=466554168000.0,
            expected_plan=5,
            expected_materialize_min=1,
            expected_cache_bytes_min=1,
        ),
    ]
