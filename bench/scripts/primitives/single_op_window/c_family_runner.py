"""C/C++ runners for single-op window benchmark."""

from __future__ import annotations

import pathlib
import shutil
import statistics
import tempfile
from typing import Optional

from .builders import make_c_source
from .common import REPO_ROOT, parse_lines, run_checked
from .models import WindowResult


def run_c_like(
    language: str,
    operator: str,
    loops: int,
    batch: int,
    runs: int,
    a_lit: str,
    b_lit: str,
) -> Optional[WindowResult]:
    compiler = "clang++" if language == "cpp" else "clang"
    if shutil.which(compiler) is None:
        return None

    floor_samples: list[float] = []
    raw_samples: list[float] = []
    checksum = ""

    with tempfile.TemporaryDirectory(prefix=f"single-op-{language}-") as tmp:
        tmpdir = pathlib.Path(tmp)
        src = tmpdir / ("main.cpp" if language == "cpp" else "main.c")
        bin_path = tmpdir / "single_op.bin"
        make_c_source(src, operator, a_lit, b_lit)

        compile_cmd = [
            compiler,
            "-O3",
            "-DNDEBUG",
            "-march=native",
            "-mtune=native",
            "-funroll-loops",
            f"-DLOOP_COUNT={loops}",
            f"-DBATCH_COUNT={batch}",
            str(src),
            "-lm",
            "-o",
            str(bin_path),
        ]
        run_checked(compile_cmd, REPO_ROOT)

        for _ in range(runs):
            proc = run_checked([str(bin_path)], REPO_ROOT)
            lines = parse_lines(proc.stdout)
            if len(lines) < 3:
                raise RuntimeError(f"unexpected {language} output: {proc.stdout!r}")
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
        language=language,
        mode="native",
        primitive="f64",
        operator=operator,
        loops=loops,
        batch=batch,
        runs=runs,
        floor_ns=floor_ns,
        raw_ns=raw_ns,
        net_ns=net_ns,
        checksum=checksum,
    )
