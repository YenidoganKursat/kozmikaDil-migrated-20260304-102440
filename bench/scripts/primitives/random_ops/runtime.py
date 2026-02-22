"""Runtime execution controllers for random primitive benchmark profiles."""

from __future__ import annotations

import pathlib
import statistics
import subprocess
import tempfile
import time

from .constants import is_wide_primitive
from .io_utils import parse_last_line, run_checked
from .models import OpRuntime
from .program import make_program


def build_native(program: pathlib.Path, binary: pathlib.Path, env: dict[str, str]) -> None:
    run_checked(["./k", "build", str(program), "-o", str(binary)], env=env)


def default_env_for_profile(profile: str, safety_tier: str, primitive: str) -> dict[str, str]:
    if profile == "baseline":
        return {
            "SPARK_CFLAGS": "-std=c11 -O2 -DNDEBUG",
            "SPARK_LTO": "off",
        }
    if profile == "optimized":
        if safety_tier == "hybrid":
            # Hybrid: strict semantics for wide/high-precision primitives via interpreter fallback,
            # native fast path for low-width primitives with safe optimization flags.
            _ = primitive
            return {
                "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                                "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                                "-fno-math-errno -fno-trapping-math",
                "SPARK_LTO": "thin",
            }
        if safety_tier == "strict":
            return {
                "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -fstrict-aliasing -fno-math-errno "
                                "-fno-trapping-math",
                "SPARK_LTO": "off",
            }
    raise ValueError(f"unknown profile: {profile}")


def resolve_exec_mode(primitive: str, requested_mode: str, profile: str, safety_tier: str) -> str:
    resolved_mode = requested_mode
    if primitive in ("f128", "f256", "f512"):
        # Strict correctness: high-precision float families must run via interpreter/MPFR.
        resolved_mode = "interpret"
    if profile == "optimized" and safety_tier in ("strict", "hybrid") and is_wide_primitive(primitive):
        resolved_mode = "interpret"
    return resolved_mode


def run_profile_once(
    primitive: str,
    op_name: str,
    operator: str,
    loops: int,
    runs: int,
    warmup: int,
    profile: str,
    exec_mode: str,
    checksum_mode: str,
    env: dict[str, str],
    safety_tier: str,
) -> OpRuntime:
    with tempfile.TemporaryDirectory(prefix=f"kozmika-prim-{primitive}-{op_name}-{profile}-") as tmp:
        tmpdir = pathlib.Path(tmp)
        program = tmpdir / f"{primitive}_{op_name}.k"
        binary = tmpdir / f"{primitive}_{op_name}.bin"
        make_program(program, primitive, operator, loops, checksum_mode)

        resolved_mode = resolve_exec_mode(primitive, exec_mode, profile, safety_tier)
        if resolved_mode != "interpret":
            try:
                build_native(program, binary, env)
                resolved_mode = "native"
            except subprocess.CalledProcessError:
                if exec_mode == "native":
                    raise
                resolved_mode = "interpret"

        for _ in range(warmup):
            if resolved_mode == "native":
                run_checked([str(binary)], env=env)
            else:
                run_checked(["./k", "run", "--interpret", str(program)], env=env)

        samples: list[float] = []
        checksum = ""
        for _ in range(runs):
            t0 = time.perf_counter()
            if resolved_mode == "native":
                proc = run_checked([str(binary)], env=env)
            else:
                proc = run_checked(["./k", "run", "--interpret", str(program)], env=env)
            t1 = time.perf_counter()
            samples.append(t1 - t0)
            checksum = parse_last_line(proc.stdout)

    return OpRuntime(
        primitive=primitive,
        op_name=op_name,
        operator=operator,
        profile=profile,
        exec_mode=resolved_mode,
        loops=loops,
        runs=runs,
        warmup=warmup,
        median_sec=statistics.median(samples),
        min_sec=min(samples),
        max_sec=max(samples),
        checksum_raw=checksum,
    )

