# PHASE 1 REPORT

Status: **GO** (as of 2026-02-15)

## Scope

- Ubuntu/LTS single-command toolchain bootstrap available:
  - `scripts/ubuntu_toolchain.sh`
  - `scripts/bootstrap_phase1.sh`
- Repo skeleton in place: `compiler`, `runtime`, `stdlib`, `tests`, `bench`, `docs`
- Build system with CMake + Ninja switches:
  - `SPARK_BUILD_TESTS`
  - `SPARK_BUILD_BENCHMARKS`
- Benchmark harness integrated:
  - `bench/scripts/run_phase1_baselines.sh`
  - `bench/benchmarks/{bench_scalar.c,bench_list.c,bench_matrix_elemwise.c,bench_matmul.c}`
- Correctness gates and machine-readable output in JSON/CSV
  - PASS/FAIL from program printed `pass=...`
- Reproducibility policy drafted and encoded in runner

## Measured Baselines (Phase 1)

- Run command: `bash bench/scripts/run_phase1_baselines.sh`
- Command used: `7` timed runs per benchmark (`RUNS=7`, min/max trimmed, median + drift based gate)
- Build type: `Release` (`-DCMAKE_BUILD_TYPE=Release`), ensuring optimized baselines.
- Output files:
  - `bench/results/phase1_raw.json`
  - `bench/results/phase1_raw.csv`
- Benchmarks
- `scalar` → PASS, reproducible
- `list` → PASS, reproducible
- `matrix_elemwise` → PASS, reproducible
- `matmul` → PASS, reproducible (naive C baseline)
- Optional collectors:
  - `hyperfine.json` (if `hyperfine` installed)
  - `perf_stats.txt` (if `perf` installed)

## Reproducibility Check Result

- Drift rule (`max(|ti - median| / median) * 100 <= 3.0`) is met for all benchmarks.
- No correctness failures (all `pass=PASS`).

## GO/NO-GO Decision

- **GO** for Phase 2:
  - Objective measurement rail is working before any language implementation.
  - Baselines and gates allow objective comparison of future compiler/runtime changes.
  - Reproducibility + output formats are in place and functioning.
