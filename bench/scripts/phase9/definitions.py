from dataclasses import dataclass
from typing import List


@dataclass(frozen=True)
class ProgramCase:
    name: str
    source: str


def phase9_programs() -> List[ProgramCase]:
    return [
        ProgramCase(name="spawn_join_overhead", source="spawn_join_overhead.k"),
        ProgramCase(name="channel_throughput", source="channel_throughput.k"),
        ProgramCase(name="parallel_for_scaling", source="parallel_for_scaling.k"),
        ProgramCase(name="par_reduce_chunk", source="par_reduce_chunk.k"),
    ]


def phase9_thread_sweep() -> List[int]:
    return [1, 2, 4, 8]


def phase9_chunk_sweep() -> List[int]:
    return [64, 128, 256, 512]
