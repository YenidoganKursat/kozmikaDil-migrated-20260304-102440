# Primitive Scalar Benchmarks

Rule: one script = one primitive benchmark task.

Modular controller layout:
- `benchmark_primitive_ops_random.py`
  - thin entrypoint only
  - runtime/controller modules:
    - `random_ops/cli.py` (argument parsing + orchestration)
    - `random_ops/runtime.py` (baseline/optimized profile runners)
    - `random_ops/program.py` (Kozmika benchmark program generator)
    - `random_ops/checksum.py` (tolerance/checksum policy)
    - `random_ops/io_utils.py` (subprocess + csv/json helpers)
- `benchmark_single_op_window_crosslang.py`
  - thin entrypoint only
  - cross-language controller modules:
    - `single_op_window/cli.py`
    - `single_op_window/kozmika_runner.py`
    - `single_op_window/c_family_runner.py`
    - `single_op_window/csharp_runner.py`
    - `single_op_window/builders.py`
    - `single_op_window/common.py`
  - supports batched operation windows via `--batch` (default `1`):
    - measures `repeat(batch){ c = a op b }` and subtracts assign-only floor path per op.
    - useful for stabilizing very small ns/op measurements without changing operation semantics.

- `benchmark_int_crosslang_fair_runtime.py`
  - thin entrypoint only
  - fair integer runtime controller modules:
    - `int_crosslang_fair/cli.py`
    - `int_crosslang_fair/runners.py`
    - `int_crosslang_fair/builders.py`
    - `int_crosslang_fair/common.py`
  - measures full-loop runtime (not per-iteration timer windows) with deterministic integer streams.
  - reports:
    - Kozmika i-series (`i8..i512`) for `interpret` and/or `native` modes,
    - cross-language i64 (`c`, `cpp`, `go`, `csharp`, `java`) when toolchains are available.
  - outputs JSON: `bench/results/primitives/int_crosslang_fair_runtime.json`

- `benchmark_float_crosslang_fair_runtime.py`
  - thin entrypoint only
  - fair float runtime controller modules:
    - `float_crosslang_fair/cli.py`
    - `float_crosslang_fair/runners.py`
    - `float_crosslang_fair/builders.py`
    - `float_crosslang_fair/common.py`
  - measures full-loop runtime (not per-iteration timer windows) with deterministic float streams.
  - reports:
    - Kozmika f-series (`f8..f512`) for `interpret` and/or `native` modes,
    - cross-language f64 (`c`, `cpp`, `go`, `csharp`, `java`) when toolchains are available.
  - outputs JSON: `bench/results/primitives/float_crosslang_fair_runtime.json`

- `benchmark_primitive_scalar_core.py`
  - Shared helper for timing and JSON output.
  - Runs one scalar loop benchmark for one primitive.
  - Execution policy:
    - tries `k build` once and runs the produced native binary for timing (build time excluded),
    - falls back to `k run --interpret` if native codegen is not available.
  - Benchmark kernels use `bench_tick()` runtime seed to avoid compile-time over-optimization artifacts.

Per-primitive wrappers (single responsibility):
- `benchmark_i8_scalar_runtime.py`
- `benchmark_i16_scalar_runtime.py`
- `benchmark_i32_scalar_runtime.py`
- `benchmark_i64_scalar_runtime.py`
- `benchmark_i128_scalar_runtime.py`
- `benchmark_i256_scalar_runtime.py`
- `benchmark_i512_scalar_runtime.py`
- `benchmark_f8_scalar_runtime.py`
- `benchmark_f16_scalar_runtime.py`
- `benchmark_bf16_scalar_runtime.py`
- `benchmark_f32_scalar_runtime.py`
- `benchmark_f64_scalar_runtime.py`
- `benchmark_f128_scalar_runtime.py`
- `benchmark_f256_scalar_runtime.py`
- `benchmark_f512_scalar_runtime.py`

Matrix-free operator sweep:
- `benchmark_float_ops_100m.py`
  - runs `+, -, *, /, %, ^` separately for `f8..f512`
  - default loop count: `100_000_000`
  - performance layers:
    - `--perf-layer strict`: strict repeat semantics, no aggregate shortcut
    - `--perf-layer tier-max`: layered perf mode
      - native codegen: `SPARK_REPEAT_AGGREGATE=1`
      - high precision interpreter fallback: `SPARK_HP_REPEAT_AGGREGATE=1`
      - build profile: `SPARK_OPT_PROFILE=layered-max`
  - outputs JSON: `bench/results/primitives/float_ops_100m.json`

All-primitive random operator sweep (baseline vs optimized):
- `benchmark_primitive_ops_random.py`
  - runs `+, -, *, /, %` separately for:
    - int family: `i8..i512`
    - float family: `f8..f512` (+ `bf16`)
  - generates deterministic pseudo-random `x/y` streams per iteration.
  - default checksum mode is `accumulate`:
    - `tmp = x <op> y`
    - float family: `acc = acc + f64(tmp)`
    - int family: rolling integer hash checksum (`acc = (acc * C + i64(tmp)) % P`)
    - keeps loop-carried dependency to reduce dead-code elimination risk.
  - optional `--checksum-mode last` keeps only the last op result.
  - compares two profiles in one run:
    - baseline profile (default interpreter execution)
    - optimized profile (default native execution)
  - safety tiers:
    - `--safety-tier strict`: correctness-first (`-O3`, no approximate high-precision native)
    - `--safety-tier hybrid` (default): low-width types native fast path, wide/high-precision types interpreter strict path
  - tolerant checksum policy fields are emitted per row:
    - `checksums_match_tolerant`
    - `checksum_abs_diff`
    - `checksum_rel_diff`
    - `checksum_tolerance`
  - optional `--fail-on-mismatch` exits non-zero when tolerant checksum policy fails.
  - outputs (prefix configurable via `--out-prefix`):
    - `bench/results/primitives/<prefix>.json`
    - `bench/results/primitives/<prefix>.csv`

BigDecimal correctness checks:
- `check_float_ops_bigdecimal.py`
  - compares Kozmika output against Java `BigDecimal` reference (result-level check).
  - outputs JSON: `bench/results/primitives/float_ops_bigdecimal_check_run.json`

- `check_float_ops_stepwise_bigdecimal.py`
  - stepwise check (every iteration) with deterministic random `a/b`.
  - uses binary-exact random inputs (`((seed % 4097) - 2048) / 256.0`) to reduce decimal input noise.
  - computes max abs/rel error and `max_eps1_ratio` (`abs_error / epsilon@1`) versus Java `BigDecimal`.
  - for `f128/f256/f512`, default path replaces Java reference rows with MPFR high-precision stepwise reference
    (`bench/scripts/primitives/mpfr_stepwise_reference.cpp`) to avoid double-based reference limits.
  - applies primitive-specific safe input bounds to avoid non-finite noise (`f8/f16`).
  - optional `--skip-kozmika` to skip runtime checksum cross-check.
  - optional `--skip-mpfr-high-ref` to force legacy Java-only reference rows.
  - optional `--python-crosscheck` for second reference path (Python `decimal`).
  - outputs JSON: `bench/results/primitives/float_ops_stepwise_bigdecimal_check.json`
  - outputs Markdown table: `bench/results/primitives/float_ops_stepwise_bigdecimal_table.md`

- `compare_dataset_100m_vs_bigdecimal.py`
  - generates one deterministic dataset and computes aggregate operator sums in:
    - Kozmika runtime (`k run`)
    - Java `BigDecimal` reference
  - reports absolute/relative differences per primitive (`f8,f16,f32,f64,f128,f256,f512`).
  - default loops: `100_000_000`.
  - outputs JSON: `bench/results/primitives/dataset_100m_vs_bigdecimal.json`

Hybrid one-shot speed + BigDecimal pipeline:
- `run_hybrid_speed_bigdecimal.py`
  - runs `benchmark_primitive_ops_random.py` with:
    - baseline: `interpret`
    - optimized: `auto`
    - configurable `--safety-tier strict|hybrid`
  - then runs `check_float_ops_stepwise_bigdecimal.py`
  - produces:
    - `bench/results/primitives/<out-prefix>_report.json`
    - `bench/results/primitives/<out-prefix>_report.md`

Integer correctness checks (Python reference):
- `validate_int_ops_python.py`
  - validates `i8..i512` operators (`+,-,*,/,%,^`) on deterministic random streams.
  - mirrors current runtime semantics (signed saturating arithmetic, C-style remainder).
  - optional `--include-extremes`:
    - deterministic boundary vectors (min/max/near-boundary/zero/sign flips),
    - plus extra seeded edge-random vectors per primitive/operator.
  - outputs JSON: `bench/results/primitives/int_ops_python_validation.json`

Integer init/op cross-language checks (Java BigInteger reference):
- `validate_int_init_ops_java_bigint.py`
  - validates `i8..i512` constructor/init behavior and integer ops (`+,-,*,%,^`) against Java `BigInteger`.
  - includes deterministic boundary/out-of-range vectors and seeded random vectors.
  - models signed clamping semantics for each integer width.
  - outputs JSON: `bench/results/primitives/int_init_ops_java_bigint_validation.json`

Float extreme-case cross-language checks:
- `validate_float_extreme_bigdecimal.py`
  - validates `f8..f512` operators (`+,-,*,/,%,^`) on deterministic extreme vectors.
  - reference backends:
    - low families (`f8/f16/f32/f64`): Java `BigDecimal` primary, Python `decimal` cross-check,
    - high families (`f128/f256/f512`): MPFR strict primary, Java/Python informational cross-check.
  - applies primitive-aware strict tolerance gates and reports per primitive/operator max errors.
  - outputs JSON: `bench/results/primitives/float_extreme_bigdecimal_validation.json`

Baseline convention:
- Use `benchmark_all_primitives_cycle.py --reset-baseline` once to set the 1x baseline for all primitives.
- Run `benchmark_all_primitives_cycle.py` again after optimizations to get `speedup_vs_1x`.
- Per-primitive `*_baseline.json` and `*_latest.json` are written under `bench/results/primitives`.
- `benchmark_all_primitives_cycle.py --mode interpret|native|auto`:
  - `interpret`: force interpreter,
  - `native`: force AOT native (fails if codegen unavailable),
  - `auto`: try native first, fallback to interpreter.
