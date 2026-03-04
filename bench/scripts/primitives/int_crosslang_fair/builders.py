"""Program/source builders for fair integer cross-language benchmarks."""

from __future__ import annotations

import pathlib


def _lane_seed(index: int) -> int:
    return (123456789 + index * 362437) & 0x7FFFFFFF


def _domain_bits_for_primitive(bits: int, operator: str) -> int:
    # Keep values in a range that avoids overflow-driven saturation artifacts
    # while preserving a dense deterministic stream for fair cross-language runs.
    if operator == "^":
        if bits <= 8:
            return 3
        if bits <= 16:
            return 4
        if bits <= 32:
            return 6
        return 7
    if operator == "*":
        if bits <= 8:
            return 4
        if bits <= 16:
            return 8
        if bits <= 32:
            return 12
        return 16
    if bits <= 8:
        return 6
    if bits <= 16:
        return 12
    if bits <= 32:
        return 18
    return 20


def op_domain_for_bits(bits: int, operator: str, pow_profile: str = "generic") -> tuple[int, int]:
    # Returns:
    # - signed mask bits for x/y generation via bitmask
    # - exponent mask for pow generation via bitmask
    dom_bits = _domain_bits_for_primitive(bits, operator)
    if operator == "^":
        # generic: wider exponent domain; hot: x^0/x^1 hot-path profiling.
        if pow_profile == "hot":
            exp_mask = 1
        else:
            if bits <= 8:
                exp_mask = 3
            elif bits <= 16:
                exp_mask = 7
            else:
                exp_mask = 7
        return (dom_bits, exp_mask)
    return (dom_bits, 0)


def make_kozmika_program(
    path: pathlib.Path,
    primitive: str,
    bits: int,
    operator: str,
    loops: int,
    lanes: int = 1,
    pow_profile: str = "generic",
    use_runtime_kernel: bool = False,
) -> None:
    if use_runtime_kernel:
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
    dom_bits, exp_mask = op_domain_for_bits(bits, operator, pow_profile)
    x_mask = (1 << dom_bits) - 1
    x_bias = 1 << (dom_bits - 1)
    seed_lines: list[str] = []
    acc_lines: list[str] = []
    body_lines: list[str] = []
    for lane in range(lanes):
        seed_lines.append(f"seed_x{lane} = i64({_lane_seed(lane)})")
        acc_lines.append(f"acc{lane} = {primitive}(0)")
        y_expr = f"{primitive}((mix{lane} % {x_mask + 1}) - {x_bias})"
        if operator == "^":
            y_expr = f"{primitive}(mix{lane} % {exp_mask + 1})"
        elif operator in ("/", "%"):
            # Keep denominator strictly non-zero without an extra branch in the hot loop.
            y_expr = f"{primitive}((mix{lane} % {x_mask + 1}) + 1)"
        body_lines.extend(
            [
                f"  seed_x{lane} = (seed_x{lane} * 1664525 + 1013904223) % 2147483648",
                f"  x{lane} = {primitive}((seed_x{lane} % {x_mask + 1}) - {x_bias})",
                f"  mix{lane} = seed_x{lane} * 22695477 + 1",
                f"  y{lane} = {y_expr}",
                f"  acc{lane} = {primitive}(x{lane} {operator} y{lane})",
            ]
        )
    checksum_terms = " + ".join(f"i64(acc{lane})" for lane in range(lanes))
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
    dom_bits, exp_mask = op_domain_for_bits(64, operator, pow_profile)
    x_mask = (1 << dom_bits) - 1
    x_bias = 1 << (dom_bits - 1)
    if operator == "^":
        op_expr_t = "pow_i64({x}, {y})"
        y_setup_t = f"(long long)({{mix}} & {exp_mask}ULL)"
    elif operator in ("/", "%"):
        op_expr_t = {
            "/": "{x} / {y}",
            "%": "{x} % {y}",
        }[operator]
        y_setup_t = f"(long long)({{mix}} & {x_mask}ULL) + 1LL"
    else:
        op_expr_t = {
            "+": "{x} + {y}",
            "-": "{x} - {y}",
            "*": "{x} * {y}",
            "/": "{x} / {y}",
            "%": "{x} % {y}",
        }[operator]
        y_setup_t = f"(long long)({{mix}} & {x_mask}ULL) - {x_bias}LL"

    seed_decls: list[str] = []
    acc_decls: list[str] = []
    body_lines: list[str] = []
    checksum_lines: list[str] = []
    for lane in range(lanes):
        seed_decls.append(f"  long long seed_x{lane} = {_lane_seed(lane)}LL;")
        acc_decls.append(f"  volatile long long acc{lane} = 0LL;")
        body_lines.extend(
            [
                f"    seed_x{lane} = (seed_x{lane} * 1664525LL + 1013904223LL) & 2147483647LL;",
                f"    long long x{lane} = (long long)(seed_x{lane} & {x_mask}ULL) - {x_bias}LL;",
                f"    long long mix{lane} = seed_x{lane} * 22695477LL + 1LL;",
                f"    long long y{lane} = {y_setup_t.format(mix=f'mix{lane}')};",
                f"    acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')};",
            ]
        )
        checksum_lines.append(f"  checksum ^= (unsigned long long)acc{lane};")

    code = f"""#include <stdint.h>
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

static inline long long pow_i64(long long base, long long exp) {{
  long long out = 1;
  long long f = base;
  unsigned long long e = (unsigned long long)exp;
  while (e > 0ULL) {{
    if ((e & 1ULL) != 0ULL) out *= f;
    e >>= 1ULL;
    if (e > 0ULL) f *= f;
  }}
  return out;
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
  unsigned long long checksum = 0ULL;
{chr(10).join(checksum_lines)}
  printf("%llu\\n", (unsigned long long)(end - start));
  printf("%lld\\n", (long long)checksum);
  return 0;
}}
"""
    path.write_text(code, encoding="utf-8")


def make_go_source(
    path: pathlib.Path, operator: str, loops: int, lanes: int = 1, pow_profile: str = "generic"
) -> None:
    lanes = max(1, lanes)
    dom_bits, exp_mask = op_domain_for_bits(64, operator, pow_profile)
    x_mask = (1 << dom_bits) - 1
    x_bias = 1 << (dom_bits - 1)
    if operator == "^":
        op_expr_t = "powI64({x}, {y})"
        y_setup_t = f"int64({{mix}} & {exp_mask})"
    elif operator in ("/", "%"):
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"int64({{mix}}&{x_mask}) + 1"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"int64({{mix}}&{x_mask}) - {x_bias}"
    seed_lines = [f"    seedX{lane} := int64({_lane_seed(lane)})" for lane in range(lanes)]
    acc_lines = [f"    acc{lane} := int64(0)" for lane in range(lanes)]
    body_lines: list[str] = []
    for lane in range(lanes):
        body_lines.extend(
            [
                f"        seedX{lane} = (seedX{lane}*1664525 + 1013904223) & 2147483647",
                f"        x{lane} := int64(seedX{lane}&{x_mask}) - {x_bias}",
                f"        mix{lane} := seedX{lane}*22695477 + 1",
                f"        y{lane} := {y_setup_t.format(mix=f'mix{lane}')}",
                f"        acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')}",
            ]
        )
    checksum_lines = [f"    checksum ^= uint64(acc{lane})" for lane in range(lanes)]
    code = f"""package main
import (
    "fmt"
    "time"
)

func powI64(base int64, exp int64) int64 {{
    out := int64(1)
    f := base
    e := uint64(exp)
    for e > 0 {{
        if (e & 1) != 0 {{
            out *= f
        }}
        e >>= 1
        if e > 0 {{
            f *= f
        }}
    }}
    return out
}}

func main() {{
    loops := {loops}
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
    start := time.Now().UnixNano()
    for i := 0; i < loops; i++ {{
{chr(10).join(body_lines)}
    }}
    elapsed := time.Now().UnixNano() - start
    checksum := uint64(0)
{chr(10).join(checksum_lines)}
    fmt.Println(elapsed)
    fmt.Println(int64(checksum))
}}
"""
    path.write_text(code, encoding="utf-8")


def make_java_source(
    path: pathlib.Path, operator: str, loops: int, lanes: int = 1, pow_profile: str = "generic"
) -> None:
    lanes = max(1, lanes)
    dom_bits, exp_mask = op_domain_for_bits(64, operator, pow_profile)
    x_mask = (1 << dom_bits) - 1
    x_bias = 1 << (dom_bits - 1)
    if operator == "^":
        op_expr_t = "powI64({x}, {y})"
        y_setup_t = f"({{mix}} & {exp_mask}L)"
    elif operator in ("/", "%"):
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"({{mix}} & {x_mask}L) + 1L"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"({{mix}} & {x_mask}L) - {x_bias}L"
    seed_lines = [f"    long seedX{lane} = {_lane_seed(lane)}L;" for lane in range(lanes)]
    acc_lines = [f"    long acc{lane} = 0L;" for lane in range(lanes)]
    body_lines: list[str] = []
    for lane in range(lanes):
        body_lines.extend(
            [
                f"      seedX{lane} = (seedX{lane} * 1664525L + 1013904223L) & 2147483647L;",
                f"      long x{lane} = (seedX{lane} & {x_mask}L) - {x_bias}L;",
                f"      long mix{lane} = seedX{lane} * 22695477L + 1L;",
                f"      long y{lane} = {y_setup_t.format(mix=f'mix{lane}')};",
                f"      acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')};",
            ]
        )
    checksum_lines = [f"    checksum ^= acc{lane};" for lane in range(lanes)]
    code = f"""public final class Main {{
  private static long powI64(long base, long exp) {{
    long out = 1L;
    long f = base;
    long e = exp;
    while (e > 0L) {{
      if ((e & 1L) != 0L) out *= f;
      e >>= 1;
      if (e > 0L) f *= f;
    }}
    return out;
  }}

  public static void main(String[] args) {{
    final int loops = {loops};
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
    long start = System.nanoTime();
    for (int i = 0; i < loops; i++) {{
{chr(10).join(body_lines)}
    }}
    long elapsed = System.nanoTime() - start;
    long checksum = 0L;
{chr(10).join(checksum_lines)}
    System.out.println(elapsed);
    System.out.println(checksum);
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
    dom_bits, exp_mask = op_domain_for_bits(64, operator, pow_profile)
    x_mask = (1 << dom_bits) - 1
    x_bias = 1 << (dom_bits - 1)
    if operator == "^":
        op_expr_t = "PowI64({x}, {y})"
        y_setup_t = f"({{mix}} & {exp_mask}L)"
    elif operator in ("/", "%"):
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"({{mix}} & {x_mask}L) + 1L"
    else:
        op_expr_t = f"{{x}} {operator} {{y}}"
        y_setup_t = f"({{mix}} & {x_mask}L) - {x_bias}L"
    seed_lines = [f"    long seedX{lane} = {_lane_seed(lane)}L;" for lane in range(lanes)]
    acc_lines = [f"    long acc{lane} = 0L;" for lane in range(lanes)]
    body_lines: list[str] = []
    for lane in range(lanes):
        body_lines.extend(
            [
                f"      seedX{lane} = (seedX{lane} * 1664525L + 1013904223L) & 2147483647L;",
                f"      long x{lane} = (seedX{lane} & {x_mask}L) - {x_bias}L;",
                f"      long mix{lane} = seedX{lane} * 22695477L + 1L;",
                f"      long y{lane} = {y_setup_t.format(mix=f'mix{lane}')};",
                f"      acc{lane} = {op_expr_t.format(x=f'x{lane}', y=f'y{lane}')};",
            ]
        )
    checksum_lines = [f"    checksum ^= acc{lane};" for lane in range(lanes)]
    path.write_text(
        f"""using System;
using System.Diagnostics;
static class Program {{
  static long PowI64(long b, long e) {{
    long outv = 1;
    long f = b;
    while (e > 0) {{
      if ((e & 1) != 0) outv *= f;
      e >>= 1;
      if (e > 0) f *= f;
    }}
    return outv;
  }}

  static int Main() {{
    const int loops = {loops};
{chr(10).join(seed_lines)}
{chr(10).join(acc_lines)}
    long start = Stopwatch.GetTimestamp();
    for (int i = 0; i < loops; i++) {{
{chr(10).join(body_lines)}
    }}
    long end = Stopwatch.GetTimestamp();
    long checksum = 0L;
{chr(10).join(checksum_lines)}
    double elapsedNs = (end - start) * 1_000_000_000.0 / Stopwatch.Frequency;
    Console.WriteLine(elapsedNs.ToString("R"));
    Console.WriteLine(checksum);
    return 0;
  }}
}}
""",
        encoding="utf-8",
    )
