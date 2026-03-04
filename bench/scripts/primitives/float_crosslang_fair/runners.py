"""Language runners for fair float cross-language benchmark."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile

from .builders import (
    make_c_family_source,
    make_csharp_project,
    make_csharp_source,
    make_go_source,
    make_java_source,
    make_kozmika_program,
)
from .common import BenchRow, REPO_ROOT, run_checked, runtime_env


def _timed_runs(
    cmd: list[str],
    loops: int,
    lanes: int,
    runs: int,
    warmup: int,
    env: dict[str, str],
    parse_elapsed_and_checksum,
    cwd: pathlib.Path | None = None,
) -> BenchRow:
    for _ in range(warmup):
        run_checked(cmd, cwd=cwd, env=env)
    samples_ns_op: list[float] = []
    checksum = ""
    for _ in range(runs):
        proc = run_checked(cmd, cwd=cwd, env=env)
        elapsed_ns, checksum = parse_elapsed_and_checksum(proc.stdout)
        samples_ns_op.append(elapsed_ns / float(loops * max(1, lanes)))
    samples_ns_op.sort()
    return BenchRow(ns_op=samples_ns_op[len(samples_ns_op) // 2], checksum=checksum)


def _parse_elapsed_and_checksum(stdout: str) -> tuple[float, str]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"unexpected benchmark output: {stdout!r}")
    return float(lines[-2]), lines[-1]


def _parse_kozmika_ticks_and_checksum(stdout: str) -> tuple[float, str]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(lines) < 4:
        raise RuntimeError(f"unexpected Kozmika benchmark output: {stdout!r}")
    tick_num = int(lines[-4])
    tick_den = int(lines[-3])
    elapsed_ticks = int(lines[-2])
    checksum = lines[-1]
    if tick_den == 0:
        raise RuntimeError("invalid tick scale denominator: 0")
    elapsed_ns = float(elapsed_ticks) * float(tick_num) / float(tick_den)
    return elapsed_ns, checksum


def run_kozmika(
    primitive: str,
    operator: str,
    loops: int,
    lanes: int,
    pow_profile: str,
    runs: int,
    warmup: int,
    mode: str,
    interpret_path: str = "kernel",
) -> BenchRow:
    op_tag = {
        "+": "add",
        "-": "sub",
        "*": "mul",
        "/": "div",
        "%": "mod",
        "^": "pow",
    }.get(operator, "op")
    with tempfile.TemporaryDirectory(prefix=f"kozmika-float-{primitive}-{op_tag}-") as tmp:
        tmpdir = pathlib.Path(tmp)
        source = tmpdir / "bench.k"
        binary = tmpdir / "bench.bin"
        use_runtime_kernel = False
        force_high_precision_kernel = True
        if mode == "interpret":
            if interpret_path == "kernel":
                use_runtime_kernel = True
                force_high_precision_kernel = True
            elif interpret_path == "ast":
                use_runtime_kernel = False
                force_high_precision_kernel = False
            elif interpret_path == "hybrid":
                risky_ops = {"/", "%", "^"}
                use_runtime_kernel = operator not in risky_ops
                force_high_precision_kernel = operator not in risky_ops
            else:
                use_runtime_kernel = True
                force_high_precision_kernel = True

        make_kozmika_program(
            source,
            primitive,
            operator,
            loops,
            lanes,
            pow_profile,
            use_runtime_kernel_for_all=use_runtime_kernel,
            force_high_precision_kernel=force_high_precision_kernel,
        )

        env = runtime_env()
        env["SPARK_ASSIGN_INPLACE_NUMERIC"] = "1"
        hp_kind = primitive in {"f128", "f256", "f512"}
        if mode == "native":
            if hp_kind:
                # Strict high-precision native path (no fast-math flags).
                env.update({"SPARK_OPT_PROFILE": "layered-max", "SPARK_LTO": "thin"})
                try:
                    run_checked(
                        ["./k", "build", str(source), "-o", str(binary), "--profile", "layered-max"],
                        cwd=REPO_ROOT,
                        env=env,
                    )
                    cmd = [str(binary)]
                except subprocess.CalledProcessError as exc:
                    raise RuntimeError(
                        f"native benchmark build failed for {primitive} {operator}"
                    ) from exc
            else:
                env.update(
                    {
                        "SPARK_OPT_PROFILE": "max",
                        "SPARK_CFLAGS": "-std=c11 -O3 -DNDEBUG -march=native -mtune=native "
                        "-fomit-frame-pointer -fstrict-aliasing -funroll-loops "
                        "-fvectorize -fslp-vectorize -fno-math-errno "
                        "-fno-trapping-math -fno-fast-math",
                        "SPARK_LTO": "full",
                    }
                )
                run_checked(
                    ["./k", "build", str(source), "-o", str(binary), "--profile", "max"],
                    cwd=REPO_ROOT,
                    env=env,
                )
                cmd = [str(binary)]
        else:
            cmd = ["./k", "run", str(source), "--interpret"]
        return _timed_runs(
            cmd,
            loops,
            lanes,
            runs,
            warmup,
            env=env,
            parse_elapsed_and_checksum=_parse_kozmika_ticks_and_checksum,
            cwd=REPO_ROOT,
        )


def run_c_like(
    language: str,
    operator: str,
    loops: int,
    lanes: int,
    pow_profile: str,
    runs: int,
    warmup: int,
) -> BenchRow | None:
    compiler = "clang++" if language == "cpp" else "clang"
    if shutil.which(compiler) is None:
        return None
    with tempfile.TemporaryDirectory(prefix=f"float-fair-{language}-") as tmp:
        tmpdir = pathlib.Path(tmp)
        source = tmpdir / ("main.cpp" if language == "cpp" else "main.c")
        binary = tmpdir / "bench.bin"
        make_c_family_source(source, operator, loops, lanes, pow_profile)
        run_checked(
            [
                compiler,
                "-O3",
                "-DNDEBUG",
                "-march=native",
                "-mtune=native",
                "-funroll-loops",
                "-fno-math-errno",
                "-fno-trapping-math",
                "-fno-fast-math",
                str(source),
                "-o",
                str(binary),
                "-lm",
            ],
            cwd=REPO_ROOT,
            env=runtime_env(),
        )
        return _timed_runs(
            [str(binary)],
            loops,
            lanes,
            runs,
            warmup,
            env=runtime_env(),
            parse_elapsed_and_checksum=_parse_elapsed_and_checksum,
            cwd=REPO_ROOT,
        )


def run_go(
    operator: str, loops: int, lanes: int, pow_profile: str, runs: int, warmup: int
) -> BenchRow | None:
    if shutil.which("go") is None:
        return None
    with tempfile.TemporaryDirectory(prefix="float-fair-go-") as tmp:
        tmpdir = pathlib.Path(tmp)
        source = tmpdir / "main.go"
        binary = tmpdir / "bench.bin"
        make_go_source(source, operator, loops, lanes, pow_profile)
        run_checked(
            ["go", "build", "-o", str(binary), str(source)],
            cwd=REPO_ROOT,
            env=runtime_env(),
        )
        return _timed_runs(
            [str(binary)],
            loops,
            lanes,
            runs,
            warmup,
            env=runtime_env(),
            parse_elapsed_and_checksum=_parse_elapsed_and_checksum,
            cwd=REPO_ROOT,
        )


def run_java(
    operator: str, loops: int, lanes: int, pow_profile: str, runs: int, warmup: int
) -> BenchRow | None:
    if shutil.which("javac") is None or shutil.which("java") is None:
        return None
    with tempfile.TemporaryDirectory(prefix="float-fair-java-") as tmp:
        tmpdir = pathlib.Path(tmp)
        source = tmpdir / "Main.java"
        make_java_source(source, operator, loops, lanes, pow_profile)
        run_checked(["javac", str(source)], cwd=tmpdir, env=runtime_env())
        java_cmd = [
            "java",
            "-Xms256m",
            "-Xmx256m",
            "-Xbatch",
            "-cp",
            str(tmpdir),
            "Main",
        ]
        return _timed_runs(
            java_cmd,
            loops,
            lanes,
            runs,
            warmup,
            env=runtime_env(),
            parse_elapsed_and_checksum=_parse_elapsed_and_checksum,
            cwd=tmpdir,
        )


def run_csharp(
    operator: str, loops: int, lanes: int, pow_profile: str, runs: int, warmup: int
) -> BenchRow | None:
    if shutil.which("dotnet") is None:
        return None
    with tempfile.TemporaryDirectory(prefix="float-fair-csharp-") as tmp:
        tmpdir = pathlib.Path(tmp)
        project = tmpdir / "Bench.csproj"
        source = tmpdir / "Program.cs"
        make_csharp_project(project)
        make_csharp_source(source, operator, loops, lanes, pow_profile)
        run_checked(["dotnet", "build", "-c", "Release"], cwd=tmpdir, env=runtime_env())
        cmd = ["dotnet", "run", "-c", "Release", "--no-build"]
        return _timed_runs(
            cmd,
            loops,
            lanes,
            runs,
            warmup,
            env=runtime_env(),
            parse_elapsed_and_checksum=_parse_elapsed_and_checksum,
            cwd=tmpdir,
        )
