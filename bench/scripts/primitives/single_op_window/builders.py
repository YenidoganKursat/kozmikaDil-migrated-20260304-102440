"""Source/program builders for each language in single-op window benchmark."""

from __future__ import annotations

import pathlib


def make_kozmika_program(
    path: pathlib.Path,
    primitive: str,
    operator: str,
    loops: int,
    batch: int,
    a_lit: str,
    b_lit: str,
    tick_mode: str,
) -> None:
    op_expr = f"a {operator} b"
    is_int_primitive = primitive.startswith("i")
    # Integer `/` and `^` currently produce float in language semantics.
    # Keep measurement program type-correct for i-series.
    raw_result_primitive = "f64" if (is_int_primitive and operator in ("/", "^")) else primitive
    tick_fn = "bench_tick_raw" if tick_mode == "raw" else "bench_tick"
    header: list[str] = []
    footer: list[str] = []
    if tick_mode == "raw":
        header = [
            "tick_num = bench_tick_scale_num()",
            "tick_den = bench_tick_scale_den()",
        ]
        footer = [
            "print(tick_num)",
            "print(tick_den)",
        ]
    floor_block = ["  floor_c = a"] * batch
    raw_block = [f"  raw_c = {op_expr}"] * batch
    source = "\n".join(
        header + [
            f"a = {primitive}({a_lit})",
            f"b = {primitive}({b_lit})",
            f"floor_c = {primitive}(0)",
            f"raw_c = {raw_result_primitive}(0)",
            "i = 0",
            "floor_total = i64(0)",
            "raw_total = i64(0)",
            f"while i < {loops}:",
            f"  f1 = {tick_fn}()",
        ]
        + floor_block
        + [
            f"  f2 = {tick_fn}()",
            "  floor_total = floor_total + (f2 - f1)",
            f"  t1 = {tick_fn}()",
        ]
        + raw_block
        + [
            f"  t2 = {tick_fn}()",
            "  raw_total = raw_total + (t2 - t1)",
            "  i = i + 1",
        ]
        + footer
        + [
            "print(floor_total)",
            "print(raw_total)",
            "print(raw_c)",
            "",
        ]
    )
    path.write_text(source, encoding="utf-8")


def make_c_source(path: pathlib.Path, operator: str, a_lit: str, b_lit: str) -> None:
    op_expr = {
        "+": "a + b",
        "-": "a - b",
        "*": "a * b",
        "/": "a / b",
        "%": "fmod(a, b)",
        "^": "pow(a, b)",
    }[operator]

    code = f"""\
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline uint64_t tick_ns(void) {{
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}}

int main(void) {{
  const int loops = LOOP_COUNT;
  const int batch = BATCH_COUNT;
  volatile double a = {a_lit};
  volatile double b = {b_lit};
  volatile double c = 0.0;

  uint64_t floor_total = 0;
  uint64_t raw_total = 0;

  for (int i = 0; i < loops; ++i) {{
    const uint64_t f1 = tick_ns();
    for (int j = 0; j < batch; ++j) {{
      c = a;
    }}
    const uint64_t f2 = tick_ns();
    floor_total += (f2 - f1);
  }}

  for (int i = 0; i < loops; ++i) {{
    const uint64_t t1 = tick_ns();
    for (int j = 0; j < batch; ++j) {{
      c = {op_expr};
    }}
    const uint64_t t2 = tick_ns();
    raw_total += (t2 - t1);
  }}

  printf("%llu\\n", (unsigned long long)floor_total);
  printf("%llu\\n", (unsigned long long)raw_total);
  printf("%.17g\\n", (double)c);
  return 0;
}}
"""
    path.write_text(code, encoding="utf-8")


def make_csharp_project(path: pathlib.Path) -> None:
    path.write_text(
        """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Optimize>true</Optimize>
    <TieredCompilation>false</TieredCompilation>
  </PropertyGroup>
</Project>
""",
        encoding="utf-8",
    )


def make_csharp_program(path: pathlib.Path, operator: str, loops: int, batch: int, a_lit: str, b_lit: str) -> None:
    op_expr = {
        "+": "a + b",
        "-": "a - b",
        "*": "a * b",
        "/": "a / b",
        "%": "a % b",
        "^": "Math.Pow(a, b)",
    }[operator]
    path.write_text(
        f"""using System;
using System.Diagnostics;
using System.Globalization;
using System.Threading;

static class Program
{{
    private static double A;
    private static double B;
    private static double Sink;
    private const int Loops = {loops};
    private const int Batch = {batch};

    private static double ToNs(long ticks) => ticks * 1_000_000_000.0 / Stopwatch.Frequency;

    public static int Main()
    {{
        A = double.Parse("{a_lit}", CultureInfo.InvariantCulture);
        B = double.Parse("{b_lit}", CultureInfo.InvariantCulture);

        long floorTotal = 0;
        long rawTotal = 0;
        for (int i = 0; i < Loops; i++)
        {{
            double a = Volatile.Read(ref A);
            long f1 = Stopwatch.GetTimestamp();
            for (int j = 0; j < Batch; j++)
            {{
                Volatile.Write(ref Sink, a);
            }}
            long f2 = Stopwatch.GetTimestamp();
            floorTotal += (f2 - f1);
        }}

        for (int i = 0; i < Loops; i++)
        {{
            double a = Volatile.Read(ref A);
            double b = Volatile.Read(ref B);
            long t1 = Stopwatch.GetTimestamp();
            for (int j = 0; j < Batch; j++)
            {{
                double c = {op_expr};
                Volatile.Write(ref Sink, c);
            }}
            long t2 = Stopwatch.GetTimestamp();
            rawTotal += (t2 - t1);
        }}

        Console.WriteLine(ToNs(floorTotal));
        Console.WriteLine(ToNs(rawTotal));
        Console.WriteLine(Volatile.Read(ref Sink).ToString("R", CultureInfo.InvariantCulture));
        return 0;
    }}
}}
""",
        encoding="utf-8",
    )
