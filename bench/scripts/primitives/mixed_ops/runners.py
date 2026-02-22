"""Backend-specific measurement runners."""

from __future__ import annotations

import pathlib
import statistics
import tempfile
import time
from typing import Dict, List

from .common import parse_last_line, run_checked
from .programs import make_builtin_program, make_operator_program


def benchmark_builtin(
    repo_root: pathlib.Path,
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    warmup: int,
    runs: int,
    seed_x: int,
    seed_y: int,
) -> Dict[str, object]:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-mixed-{primitive}-{op_name}-builtin-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}.k"
        make_builtin_program(program, primitive, operator, loops, seed_x, seed_y)

        for _ in range(warmup):
            run_checked(repo_root, ["./k", "run", "--interpret", str(program)])

        samples: List[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            proc = run_checked(repo_root, ["./k", "run", "--interpret", str(program)])
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    return _result_row("builtin", primitive, op_name, operator, loops, warmup, runs, samples, checksum)


def benchmark_native(
    repo_root: pathlib.Path,
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    warmup: int,
    runs: int,
) -> Dict[str, object]:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-mixed-{primitive}-{op_name}-native-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}.k"
        binary = pathlib.Path(tmp) / f"{primitive}_{op_name}.bin"
        make_operator_program(program, primitive, operator, loops)

        native_env = {
            "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                            "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                            "-fno-math-errno -fno-trapping-math",
            "SPARK_LTO": "off",
        }
        run_checked(repo_root, ["./k", "build", str(program), "-o", str(binary)], env=native_env)

        for _ in range(warmup):
            run_checked(repo_root, [str(binary)], env=native_env)

        samples: List[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            proc = run_checked(repo_root, [str(binary)], env=native_env)
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    return _result_row("native", primitive, op_name, operator, loops, warmup, runs, samples, checksum)


def benchmark_interpret(
    repo_root: pathlib.Path,
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    warmup: int,
    runs: int,
) -> Dict[str, object]:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-mixed-{primitive}-{op_name}-interpret-") as tmp:
        program = pathlib.Path(tmp) / f"{primitive}_{op_name}.k"
        make_operator_program(program, primitive, operator, loops)

        for _ in range(warmup):
            run_checked(repo_root, ["./k", "run", "--interpret", str(program)])

        samples: List[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            proc = run_checked(repo_root, ["./k", "run", "--interpret", str(program)])
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    return _result_row("interpret", primitive, op_name, operator, loops, warmup, runs, samples, checksum)


def _result_row(
    backend: str,
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    warmup: int,
    runs: int,
    samples: List[float],
    checksum: str,
) -> Dict[str, object]:
    return {
        "backend": backend,
        "primitive": primitive,
        "op_name": op_name,
        "operator": operator,
        "loops": loops,
        "warmup": warmup,
        "runs": runs,
        "median_sec": statistics.median(samples),
        "min_sec": min(samples),
        "max_sec": max(samples),
        "checksum": checksum,
    }
