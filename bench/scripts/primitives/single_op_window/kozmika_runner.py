"""Kozmika runner for single-op window benchmark."""

from __future__ import annotations

import pathlib
import statistics
import tempfile

from .builders import make_kozmika_program
from .common import REPO_ROOT, parse_lines, run_checked
from .models import WindowResult


def run_kozmika(
    primitive: str,
    operator: str,
    loops: int,
    batch: int,
    runs: int,
    mode: str,
    a_lit: str,
    b_lit: str,
    tick_mode: str,
) -> WindowResult:
    floor_samples: list[float] = []
    raw_samples: list[float] = []
    checksum = ""

    with tempfile.TemporaryDirectory(prefix=f"single-op-{primitive}-") as tmp:
        tmpdir = pathlib.Path(tmp)
        program = tmpdir / "single_op.k"
        binary = tmpdir / "single_op.bin"
        make_kozmika_program(program, primitive, operator, loops, batch, a_lit, b_lit, tick_mode)

        env: dict[str, str] = {
            "OPENBLAS_NUM_THREADS": "1",
            "OMP_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
            "VECLIB_MAXIMUM_THREADS": "1",
            "BLIS_NUM_THREADS": "1",
            "SPARK_ASSIGN_INPLACE_NUMERIC": "1",
        }

        if mode == "native":
            env.update(
                {
                    "SPARK_OPT_PROFILE": "max",
                    "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                    "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                    "-fvectorize -fslp-vectorize -fno-math-errno",
                    "SPARK_LTO": "full",
                }
            )
            run_checked(
                ["./k", "build", str(program), "-o", str(binary), "--profile", "max"],
                REPO_ROOT,
                env=env,
            )
            runner = [str(binary)]
        else:
            runner = ["./k", "run", str(program)]

        # one warmup for stable timer conversion
        run_checked(runner, REPO_ROOT, env=env)

        for _ in range(runs):
            proc = run_checked(runner, REPO_ROOT, env=env)
            lines = parse_lines(proc.stdout)
            if len(lines) < 3:
                raise RuntimeError(f"unexpected Kozmika output: {proc.stdout!r}")
            if tick_mode == "raw":
                if len(lines) < 5:
                    raise RuntimeError(f"unexpected Kozmika raw-tick output: {proc.stdout!r}")
                tick_num = int(lines[-5])
                tick_den = int(lines[-4])
                floor_total = int(lines[-3])
                raw_total = int(lines[-2])
                checksum = lines[-1]
                scale = float(tick_num) / float(tick_den) if tick_den != 0 else 1.0
                denom = float(loops * batch)
                floor_samples.append((floor_total * scale) / denom)
                raw_samples.append((raw_total * scale) / denom)
            else:
                floor_total = int(lines[-3])
                raw_total = int(lines[-2])
                checksum = lines[-1]
                denom = float(loops * batch)
                floor_samples.append(floor_total / denom)
                raw_samples.append(raw_total / denom)

    floor_ns = statistics.median(floor_samples)
    raw_ns = statistics.median(raw_samples)
    net_ns = max(raw_ns - floor_ns, 0.0)
    return WindowResult(
        language="kozmika",
        mode=mode,
        primitive=primitive,
        operator=operator,
        loops=loops,
        batch=batch,
        runs=runs,
        floor_ns=floor_ns,
        raw_ns=raw_ns,
        net_ns=net_ns,
        checksum=checksum,
    )
