"""Program/source builders for fair float cross-language benchmarks."""

from __future__ import annotations

import pathlib


def _lane_seed(index: int) -> int:
    return (987654321 + index * 524287) & 0x7FFFFFFF


def _domain_for_primitive(primitive: str, operator: str, pow_profile: str) -> tuple[int, int]:
    # Returns (value_mask, exponent_mask)
    if primitive == "f8":
        base = 31
    elif primitive == "f16":
        base = 255
    elif primitive == "f32":
        base = 4095
    elif primitive == "f64":
        base = 16383
    else:
        base = 65535

    if operator == "^":
        exp_mask = 3 if pow_profile == "hot" else 7
        return base, exp_mask
    return base, 0


def make_kozmika_program(
    path: pathlib.Path,
    primitive: str,
    operator: str,
    loops: int,
    lanes: int = 1,
    pow_profile: str = "generic",
    use_runtime_kernel_for_all: bool = False,
    force_high_precision_kernel: bool = True,
) -> None:
    if use_runtime_kernel_for_all or (
        force_high_precision_kernel and primitive in ("f128", "f256", "f512")
    ):
        source = f"""tick_num = bench_tick_scale_num()
tick_den = bench_tick_scale_den()
start_tick = bench_tick_raw()
checksum = bench_mixed_numeric_op("{primitive}", "{operator}", {loops}, 123456789, 362436069)
end_tick = bench_tick_raw()
print(tick_num)
print(tick_den)
print(end_tick - start_tick)
print(checksum)
"""
        path.write_text(source, encoding="utf-8")
        return

    lanes = max(1, lanes)
    value_mask, exp_mask = _domain_for_primitive(primitive, operator, pow_profile)
    bias = value_mask // 2

    seed_lines: list[str] = []
    acc_lines: list[str] = []
    body_lines: list[str] = []
    for lane in range(lanes):
        seed_lines.append(f"seed_x{lane} = i64({_lane_seed(lane)})")
        acc_lines.append(f"acc{lane} = {primitive}(0)")
        if operator == "^":
            y_expr = f"{primitive}(mix{lane} % {exp_mask + 1})"
        elif operator in ("/", "%"):
            y_expr = f"{primitive}((mix{lane} % {value_mask + 1}) + 1)"
        else:
            y_expr = f"{primitive}((mix{lane} % {value_mask + 1}) - {bias})"
        body_lines.extend(
            [
                f"  seed_x{lane} = (seed_x{lane} * 1664525 + 1013904223) % 2147483648",
                f"  x{lane} = {primitive}((seed_x{lane} % {value_mask + 1}) - {bias})",
                f"  mix{lane} = seed_x{lane} * 22695477 + 1",
                f"  y{lane} = {y_expr}",
                f"  acc{lane} = {primitive}(x{lane} {operator} y{lane})",
            ]
        )
    checksum_terms = " + ".join(f"f64(acc{lane})" for lane in range(lanes))
    source = f"""tick_num = bench_tick_scale_num()
tick_den = bench_tick_scale_den()
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
i = 0
start_tick = bench_tick_raw()
while i < {loops}:
{chr(10).join(body_lines)}
  i = i + 1
end_tick = bench_tick_raw()
print(tick_num)
print(tick_den)
print(end_tick - start_tick)
print({checksum_terms})
"""
    path.write_text(source, encoding="utf-8")


def make_c_family_source(
    path: pathlib.Path, operator: str, loops: int, lanes: int = 1, pow_profile: str = "generic"
) -> None:
    lanes = max(1, lanes)
    value_mask, exp_mask = _domain_for_primitive("f64", operator, pow_profile)
    bias = value_mask // 2
    if operator == "^":
        op_expr_t = "pow({x}, {y})"
        y_setup_t = f"(double)({{mix}} & {exp_mask}ULL)"
    elif operator == "%":
        op_expr_t = "fmod({x}, {y})"
        y_setup_t = f"(double)(({{mix}} & {value_mask}ULL) + 1ULL)"
    elif operator == "/":
        op_expr_t = "{x} / {y}"
        y_setup_t = f"(double)(({{mix}} & {value_mask}ULL) + 1ULL)"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"(double)((long long)({{mix}} & {value_mask}ULL) - {bias}LL)"

    seed_decls: list[str] = []
    acc_decls: list[str] = []
    body_lines: list[str] = []
    checksum_lines: list[str] = []
    for lane in range(lanes):
        seed_decls.append(f"  unsigned long long seed_x{lane} = {_lane_seed(lane)}ULL;")
        acc_decls.append(f"  volatile double acc{lane} = 0.0;")
        body_lines.extend(
            [
                f"    seed_x{lane} = (seed_x{lane} * 1664525ULL + 1013904223ULL) & 2147483647ULL;",
                f"    double x{lane} = (double)((long long)(seed_x{lane} & {value_mask}ULL) - {bias}LL);",
                f"    unsigned long long mix{lane} = seed_x{lane} * 22695477ULL + 1ULL;",
                f"    double y{lane} = {y_setup_t.format(mix=f'mix{lane}')};",
                f"    acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')};",
            ]
        )
        checksum_lines.append(f"  checksum += acc{lane};")

    code = f"""#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline unsigned long long tick_ns(void) {{
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
#endif
}}

int main(void) {{
  const int loops = {loops};
{chr(10).join(seed_decls)}
{chr(10).join(acc_decls)}
  const unsigned long long start = tick_ns();
  for (int i = 0; i < loops; ++i) {{
{chr(10).join(body_lines)}
  }}
  const unsigned long long end = tick_ns();
  volatile double checksum = 0.0;
{chr(10).join(checksum_lines)}
  printf("%llu\\n", (unsigned long long)(end - start));
  printf("%.17g\\n", (double)checksum);
  return 0;
}}
"""
    path.write_text(code, encoding="utf-8")


def make_go_source(
    path: pathlib.Path, operator: str, loops: int, lanes: int = 1, pow_profile: str = "generic"
) -> None:
    lanes = max(1, lanes)
    value_mask, exp_mask = _domain_for_primitive("f64", operator, pow_profile)
    bias = value_mask // 2
    if operator == "^":
        op_expr_t = "math.Pow({x}, {y})"
        y_setup_t = f"float64({{mix}} & {exp_mask})"
    elif operator == "%":
        op_expr_t = "math.Mod({x}, {y})"
        y_setup_t = f"float64(({{mix}} & {value_mask}) + 1)"
    elif operator == "/":
        op_expr_t = "{x} / {y}"
        y_setup_t = f"float64(({{mix}} & {value_mask}) + 1)"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"float64(int64({{mix}}&{value_mask}) - {bias})"

    seed_lines = [f"    seedX{lane} := uint64({_lane_seed(lane)})" for lane in range(lanes)]
    acc_lines = [f"    acc{lane} := float64(0)" for lane in range(lanes)]
    body_lines: list[str] = []
    for lane in range(lanes):
        body_lines.extend(
            [
                f"        seedX{lane} = (seedX{lane}*1664525 + 1013904223) & 2147483647",
                f"        x{lane} := float64(int64(seedX{lane}&{value_mask}) - {bias})",
                f"        mix{lane} := seedX{lane}*22695477 + 1",
                f"        y{lane} := {y_setup_t.format(mix=f'mix{lane}')}",
                f"        acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')}",
            ]
        )
    checksum_lines = [f"    checksum += acc{lane}" for lane in range(lanes)]
    imports = ['    "fmt"', '    "time"']
    if operator in ("^", "%"):
        imports.insert(1, '    "math"')
    code = f"""package main
import (
{chr(10).join(imports)}
)

func main() {{
    loops := {loops}
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
    start := time.Now().UnixNano()
    for i := 0; i < loops; i++ {{
{chr(10).join(body_lines)}
    }}
    elapsed := time.Now().UnixNano() - start
    checksum := float64(0)
{chr(10).join(checksum_lines)}
    fmt.Println(elapsed)
    fmt.Printf("%.17g\\n", checksum)
}}
"""
    path.write_text(code, encoding="utf-8")


def make_java_source(
    path: pathlib.Path, operator: str, loops: int, lanes: int = 1, pow_profile: str = "generic"
) -> None:
    lanes = max(1, lanes)
    value_mask, exp_mask = _domain_for_primitive("f64", operator, pow_profile)
    bias = value_mask // 2
    if operator == "^":
        op_expr_t = "Math.pow({x}, {y})"
        y_setup_t = f"(double)(mix{ '{lane}' } & {exp_mask}L)"
    elif operator == "%":
        op_expr_t = "{x} % {y}"
        y_setup_t = f"(double)((mix{ '{lane}' } & {value_mask}L) + 1L)"
    elif operator == "/":
        op_expr_t = "{x} / {y}"
        y_setup_t = f"(double)((mix{ '{lane}' } & {value_mask}L) + 1L)"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"(double)((mix{{lane}} & {value_mask}L) - {bias}L)"

    seed_lines = [f"    long seedX{lane} = {_lane_seed(lane)}L;" for lane in range(lanes)]
    acc_lines = [f"    double acc{lane} = 0.0;" for lane in range(lanes)]
    body_lines: list[str] = []
    for lane in range(lanes):
        lane_y = y_setup_t.replace("{lane}", str(lane))
        body_lines.extend(
            [
                f"      seedX{lane} = (seedX{lane} * 1664525L + 1013904223L) & 2147483647L;",
                f"      double x{lane} = (double)((seedX{lane} & {value_mask}L) - {bias}L);",
                f"      long mix{lane} = seedX{lane} * 22695477L + 1L;",
                f"      double y{lane} = {lane_y};",
                f"      acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')};",
            ]
        )
    checksum_lines = [f"    checksum += acc{lane};" for lane in range(lanes)]
    code = f"""public final class Main {{
  public static void main(String[] args) {{
    final int loops = {loops};
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
    long start = System.nanoTime();
    for (int i = 0; i < loops; i++) {{
{chr(10).join(body_lines)}
    }}
    long elapsed = System.nanoTime() - start;
    double checksum = 0.0;
{chr(10).join(checksum_lines)}
    System.out.println(elapsed);
    System.out.println(Double.toString(checksum));
  }}
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


def make_csharp_source(
    path: pathlib.Path, operator: str, loops: int, lanes: int = 1, pow_profile: str = "generic"
) -> None:
    lanes = max(1, lanes)
    value_mask, exp_mask = _domain_for_primitive("f64", operator, pow_profile)
    bias = value_mask // 2
    if operator == "^":
        op_expr_t = "Math.Pow({x}, {y})"
        y_setup_t = f"(double)(mix{{lane}} & {exp_mask}UL)"
    elif operator == "%":
        op_expr_t = "{x} % {y}"
        y_setup_t = f"(double)((mix{{lane}} & {value_mask}UL) + 1UL)"
    elif operator == "/":
        op_expr_t = "{x} / {y}"
        y_setup_t = f"(double)((mix{{lane}} & {value_mask}UL) + 1UL)"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"(double)((long)(mix{{lane}} & {value_mask}UL) - {bias}L)"

    seed_lines = [f"    ulong seedX{lane} = {_lane_seed(lane)}UL;" for lane in range(lanes)]
    acc_lines = [f"    double acc{lane} = 0.0;" for lane in range(lanes)]
    body_lines: list[str] = []
    for lane in range(lanes):
        lane_y = y_setup_t.replace("{lane}", str(lane))
        body_lines.extend(
            [
                f"      seedX{lane} = (seedX{lane} * 1664525UL + 1013904223UL) & 2147483647UL;",
                f"      double x{lane} = (double)((long)(seedX{lane} & {value_mask}UL) - {bias}L);",
                f"      ulong mix{lane} = seedX{lane} * 22695477UL + 1UL;",
                f"      double y{lane} = {lane_y};",
                f"      acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')};",
            ]
        )
    checksum_lines = [f"    checksum += acc{lane};" for lane in range(lanes)]
    code = f"""using System;
using System.Diagnostics;

static class Program
{{
    public static int Main()
    {{
        const int loops = {loops};
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
        long start = Stopwatch.GetTimestamp();
        for (int i = 0; i < loops; i++)
        {{
{chr(10).join(body_lines)}
        }}
        long end = Stopwatch.GetTimestamp();
        double elapsedNs = (end - start) * 1_000_000_000.0 / Stopwatch.Frequency;
        double checksum = 0.0;
{chr(10).join(checksum_lines)}
        Console.WriteLine(elapsedNs.ToString("R"));
        Console.WriteLine(checksum.ToString("R"));
        return 0;
    }}
}}
"""
    path.write_text(code, encoding="utf-8")
