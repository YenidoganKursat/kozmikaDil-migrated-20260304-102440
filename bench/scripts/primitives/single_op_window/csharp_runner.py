"""C# runner for single-op window benchmark."""

from __future__ import annotations

import pathlib
import shutil
import statistics
import tempfile
from typing import Optional

from .builders import make_csharp_program, make_csharp_project
from .common import parse_lines, run_checked
from .models import WindowResult


def run_csharp(operator: str, loops: int, batch: int, runs: int, a_lit: str, b_lit: str) -> Optional[WindowResult]:
    if shutil.which("dotnet") is None:
        return None

    with tempfile.TemporaryDirectory(prefix="single-op-csharp-") as tmp:
        tmpdir = pathlib.Path(tmp)
        project = tmpdir / "SingleOp.csproj"
        source = tmpdir / "Program.cs"
        make_csharp_project(project)
        make_csharp_program(source, operator, loops, batch, a_lit, b_lit)

        run_checked(["dotnet", "build", "-c", "Release"], tmpdir)

        floor_samples: list[float] = []
        raw_samples: list[float] = []
        checksum = ""
        for _ in range(runs):
            proc = run_checked(["dotnet", "run", "-c", "Release", "--no-build"], tmpdir)
            lines = parse_lines(proc.stdout)
            if len(lines) < 3:
                raise RuntimeError(f"unexpected csharp output: {proc.stdout!r}")
            floor_total_ns = float(lines[-3])
            raw_total_ns = float(lines[-2])
            checksum = lines[-1]
            denom = float(loops * batch)
            floor_samples.append(floor_total_ns / denom)
            raw_samples.append(raw_total_ns / denom)

    floor_ns = statistics.median(floor_samples)
    raw_ns = statistics.median(raw_samples)
    net_ns = max(raw_ns - floor_ns, 0.0)
    return WindowResult(
        language="csharp",
        mode="release",
        primitive="double",
        operator=operator,
        loops=loops,
        batch=batch,
        runs=runs,
        floor_ns=floor_ns,
        raw_ns=raw_ns,
        net_ns=net_ns,
        checksum=checksum,
    )
