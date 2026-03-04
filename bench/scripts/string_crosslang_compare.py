#!/usr/bin/env python3
"""String speed + memory comparison across Kozmika and common languages.

Outputs median ns/op and median Max RSS (KB) per language.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import statistics
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from typing import Callable, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[2]


@dataclass
class BenchRow:
    language: str
    ns_op: float
    max_rss_kb: float
    checksum: str
    runs: int
    warmup: int


def run_checked(cmd: Sequence[str], cwd: pathlib.Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        list(cmd),
        cwd=str(cwd or ROOT),
        text=True,
        capture_output=True,
        check=True,
        env=merged,
    )


def run_with_time_v(cmd: Sequence[str], cwd: pathlib.Path | None = None, env: dict[str, str] | None = None) -> tuple[str, int]:
    timed_cmd = ["/usr/bin/time", "-v", *list(cmd)]
    proc = run_checked(timed_cmd, cwd=cwd, env=env)
    m = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", proc.stderr)
    if not m:
        raise RuntimeError(f"could not parse Max RSS from stderr:\n{proc.stderr}")
    return proc.stdout, int(m.group(1))


def _parse_two_line_output(stdout: str) -> tuple[int, str]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"unexpected benchmark output: {stdout!r}")
    return int(lines[-2]), lines[-1]


def _parse_kozmika_ticks(stdout: str) -> tuple[float, str]:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(lines) < 4:
        raise RuntimeError(f"unexpected kozmika benchmark output: {stdout!r}")
    tick_num = int(lines[-4])
    tick_den = int(lines[-3])
    elapsed_ticks = int(lines[-2])
    checksum = lines[-1]
    if tick_den == 0:
        raise RuntimeError("invalid tick denominator 0")
    elapsed_ns = float(elapsed_ticks) * float(tick_num) / float(tick_den)
    return elapsed_ns, checksum


def _kozmika_program(loops: int) -> str:
    return f"""tick_num = bench_tick_scale_num()
tick_den = bench_tick_scale_den()
seed = i64(123456789)
acc = i64(0)
i = 0
start_tick = bench_tick_raw()
while i < {loops}:
  seed = (seed * 1664525 + 1013904223) % 2147483648
  a = "s" + string(seed % 1000000)
  b = "t" + string((seed * 3) % 1000000)
  c = a + b
  if c == a:
    acc = acc + 1
  else:
    acc = acc + len(c)
  i = i + 1
end_tick = bench_tick_raw()
print(tick_num)
print(tick_den)
print(end_tick - start_tick)
print(acc)
"""


def _c_program(loops: int) -> str:
    return f"""#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static inline uint64_t tick_ns(void) {{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}}

int main(void) {{
  uint64_t seed = 123456789ULL;
  long long acc = 0;
  const int loops = {loops};
  uint64_t start = tick_ns();
  for (int i = 0; i < loops; ++i) {{
    seed = (seed * 1664525ULL + 1013904223ULL) % 2147483648ULL;
    unsigned long long x = seed % 1000000ULL;
    unsigned long long y = (seed * 3ULL) % 1000000ULL;
    char a[64], b[64], c[128];
    snprintf(a, sizeof(a), "s%llu", x);
    snprintf(b, sizeof(b), "t%llu", y);
    snprintf(c, sizeof(c), "%s%s", a, b);
    if (strcmp(c, a) == 0) {{
      acc += 1;
    }} else {{
      acc += (long long)strlen(c);
    }}
  }}
  uint64_t end = tick_ns();
  printf("%llu\\n", (unsigned long long)(end - start));
  printf("%lld\\n", acc);
  return 0;
}}
"""


def _cpp_program(loops: int) -> str:
    return f"""#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

int main() {{
  std::uint64_t seed = 123456789ULL;
  long long acc = 0;
  const int loops = {loops};
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < loops; ++i) {{
    seed = (seed * 1664525ULL + 1013904223ULL) % 2147483648ULL;
    std::string a = std::string("s") + std::to_string(seed % 1000000ULL);
    std::string b = std::string("t") + std::to_string((seed * 3ULL) % 1000000ULL);
    std::string c = a + b;
    if (c == a) {{
      acc += 1;
    }} else {{
      acc += static_cast<long long>(c.size());
    }}
  }}
  auto end = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::cout << elapsed << "\\n" << acc << "\\n";
  return 0;
}}
"""


def _go_program(loops: int) -> str:
    return f"""package main
import (
  "fmt"
  "strconv"
  "time"
)
func main() {{
  seed := uint64(123456789)
  acc := int64(0)
  loops := {loops}
  start := time.Now().UnixNano()
  for i := 0; i < loops; i++ {{
    seed = (seed*1664525 + 1013904223) % 2147483648
    a := "s" + strconv.FormatUint(seed%1000000, 10)
    b := "t" + strconv.FormatUint((seed*3)%1000000, 10)
    c := a + b
    if c == a {{
      acc += 1
    }} else {{
      acc += int64(len(c))
    }}
  }}
  elapsed := time.Now().UnixNano() - start
  fmt.Println(elapsed)
  fmt.Println(acc)
}}
"""


def _java_program(loops: int) -> str:
    return f"""public final class Main {{
  public static void main(String[] args) {{
    long seed = 123456789L;
    long acc = 0L;
    int loops = {loops};
    long start = System.nanoTime();
    for (int i = 0; i < loops; i++) {{
      seed = (seed * 1664525L + 1013904223L) % 2147483648L;
      String a = "s" + Long.toString(seed % 1000000L);
      String b = "t" + Long.toString((seed * 3L) % 1000000L);
      String c = a + b;
      if (c.equals(a)) {{
        acc += 1L;
      }} else {{
        acc += c.length();
      }}
    }}
    long elapsed = System.nanoTime() - start;
    System.out.println(elapsed);
    System.out.println(acc);
  }}
}}
"""


def _python_program(loops: int) -> str:
    return f"""import time
seed = 123456789
acc = 0
loops = {loops}
start = time.perf_counter_ns()
for _ in range(loops):
    seed = (seed * 1664525 + 1013904223) % 2147483648
    a = "s" + str(seed % 1000000)
    b = "t" + str((seed * 3) % 1000000)
    c = a + b
    if c == a:
        acc += 1
    else:
        acc += len(c)
elapsed = time.perf_counter_ns() - start
print(elapsed)
print(acc)
"""


def _median(values: list[float]) -> float:
    values = sorted(values)
    return values[len(values) // 2]


def measure(
    name: str,
    run_cmd: Sequence[str],
    loops: int,
    runs: int,
    warmup: int,
    parser: Callable[[str], tuple[float, str]],
    cwd: pathlib.Path | None = None,
    env: dict[str, str] | None = None,
) -> BenchRow:
    for _ in range(warmup):
        out, _ = run_with_time_v(run_cmd, cwd=cwd, env=env)
        parser(out)

    ns_samples: list[float] = []
    rss_samples: list[int] = []
    checksum = ""
    for _ in range(runs):
        out, rss = run_with_time_v(run_cmd, cwd=cwd, env=env)
        elapsed_ns, checksum = parser(out)
        ns_samples.append(float(elapsed_ns) / float(loops))
        rss_samples.append(rss)

    return BenchRow(
        language=name,
        ns_op=_median(ns_samples),
        max_rss_kb=float(_median(rss_samples)),
        checksum=checksum,
        runs=runs,
        warmup=warmup,
    )


def require_tool(tool: str) -> None:
    if shutil.which(tool) is None:
        raise RuntimeError(f"required tool not found: {tool}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Kozmika string benchmark vs other languages")
    parser.add_argument("--loops", type=int, default=300000)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--kozmika-bin", default="./k")
    parser.add_argument("--out", default=str(ROOT / "bench" / "results" / "strings" / "string_crosslang_latest.json"))
    args = parser.parse_args()

    require_tool("/usr/bin/time")
    require_tool("python3")
    require_tool("gcc")
    require_tool("g++")
    require_tool("go")
    require_tool("javac")
    require_tool("java")

    rows: list[BenchRow] = []
    with tempfile.TemporaryDirectory(prefix="string-crosslang-") as td:
        tdir = pathlib.Path(td)

        # Kozmika source
        k_src = tdir / "bench.k"
        k_src.write_text(_kozmika_program(args.loops), encoding="utf-8")

        # Kozmika native: compile -> emit C -> gcc -O3 -> run compiled binary.
        k_c_src = tdir / "kozmika_native.c"
        k_native_bin = tdir / "kozmika_native.bin"
        run_checked(
            [
                args.kozmika_bin,
                "compile",
                str(k_src),
                "--emit-c",
                "--emit-c-out",
                str(k_c_src),
            ],
            cwd=ROOT,
        )
        run_checked(["gcc", "-O3", "-DNDEBUG", str(k_c_src), "-lm", "-o", str(k_native_bin)])
        rows.append(
            measure(
                "kozmika_native",
                [str(k_native_bin)],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=_parse_kozmika_ticks,
                cwd=ROOT,
            )
        )
        rows.append(
            measure(
                "kozmika_interpret",
                [args.kozmika_bin, "run", str(k_src), "--interpret"],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=_parse_kozmika_ticks,
                cwd=ROOT,
            )
        )

        # C
        c_src = tdir / "main.c"
        c_bin = tdir / "c_bench.bin"
        c_src.write_text(_c_program(args.loops), encoding="utf-8")
        run_checked(["gcc", "-O3", "-DNDEBUG", str(c_src), "-o", str(c_bin)])
        rows.append(
            measure(
                "c",
                [str(c_bin)],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=lambda out: (float(_parse_two_line_output(out)[0]), _parse_two_line_output(out)[1]),
            )
        )

        # C++
        cpp_src = tdir / "main.cpp"
        cpp_bin = tdir / "cpp_bench.bin"
        cpp_src.write_text(_cpp_program(args.loops), encoding="utf-8")
        run_checked(["g++", "-O3", "-DNDEBUG", str(cpp_src), "-o", str(cpp_bin)])
        rows.append(
            measure(
                "cpp",
                [str(cpp_bin)],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=lambda out: (float(_parse_two_line_output(out)[0]), _parse_two_line_output(out)[1]),
            )
        )

        # Go
        go_src = tdir / "main.go"
        go_bin = tdir / "go_bench.bin"
        go_src.write_text(_go_program(args.loops), encoding="utf-8")
        run_checked(["go", "build", "-o", str(go_bin), str(go_src)])
        rows.append(
            measure(
                "go",
                [str(go_bin)],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=lambda out: (float(_parse_two_line_output(out)[0]), _parse_two_line_output(out)[1]),
            )
        )

        # Java
        java_src = tdir / "Main.java"
        java_src.write_text(_java_program(args.loops), encoding="utf-8")
        run_checked(["javac", str(java_src)], cwd=tdir)
        rows.append(
            measure(
                "java",
                ["java", "-Xms256m", "-Xmx256m", "-cp", str(tdir), "Main"],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=lambda out: (float(_parse_two_line_output(out)[0]), _parse_two_line_output(out)[1]),
                cwd=tdir,
            )
        )

        # Python
        py_src = tdir / "main.py"
        py_src.write_text(_python_program(args.loops), encoding="utf-8")
        rows.append(
            measure(
                "python",
                ["python3", str(py_src)],
                loops=args.loops,
                runs=args.runs,
                warmup=args.warmup,
                parser=lambda out: (float(_parse_two_line_output(out)[0]), _parse_two_line_output(out)[1]),
            )
        )

    rows = sorted(rows, key=lambda r: r.ns_op)
    fastest = rows[0].ns_op
    lowest_mem = min(r.max_rss_kb for r in rows)

    payload = {
        "workload": "deterministic string concat/compare/len loop",
        "loops": args.loops,
        "runs": args.runs,
        "warmup": args.warmup,
        "rows": [
            {
                **asdict(r),
                "vs_fastest_x": (r.ns_op / fastest) if fastest > 0 else None,
                "vs_lowest_mem_x": (r.max_rss_kb / lowest_mem) if lowest_mem > 0 else None,
            }
            for r in rows
        ],
    }

    out_path = pathlib.Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
