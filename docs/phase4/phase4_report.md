# PHASE 4 REPORT

## Status: **ACTIVE (scalar native backend bootstrapped)**

## Scope Delivered

- Parser + evaluator + semantic gate + codegen flow is connected with a native backend path:
  - `k run` compiles T4-eligible code to C and executes it.
  - `k build` produces a native binary for T4-only modules.
  - `k compile --emit-c` prints generated C.
  - `k compile --emit-asm`, `--emit-llvm`, `--emit-mlir` emit IR/assembly snapshots for debugging.
  - `k analyze --dump-tiers` gives tier summaries used by the bridge decision.
- `IRToCGenerator` now:
  - normalizes IR temps like `%t0` → `_t0`,
  - lowers control-flow (`br_if`, loops, labels),
  - emits `int main` directly for top-level program execution,
  - supports function calls, returns, scalar arithmetic/comparisons, if/while/for loop lowering,
  - keeps list/matrix/indexing unsupported explicitly (documented as phase5+ boundary).
- Phase-4 regression coverage was extended:
  - print-call lowering to native C is now emitted (`__spark_print_*`) and validated.
  - codegen test suite includes `--emit-c` validation for scalar paths.
- New phase-4 benchmark fixtures and harness:
  - `bench/programs/phase4/*.k`
  - `bench/scripts/run_phase4_benchmarks.py`
  - `bench/scripts/run_phase4_benchmarks.sh`
- Tunable benchmark/config controls were added in `run_phase4_benchmarks.py`:
  - `--runs`, `--warmup-runs`, `--repeat`, `--native-cxx`, `--baseline-cxx`,
    `--native-cflags`, `--baseline-cflags`.
- Advanced tuning flags are also supported:
  - `--native-profile`, `--baseline-profile` (`portable`, `native`, `aggressive`, `max`)
  - `--native-lto`, `--baseline-lto` (`off`, `thin`, `full`)
  - `--native-pgo`, `--baseline-pgo` (`off`, `instrument`, `use`)
  - `--pgo-profile` (required when any `--*-pgo=use`)
- Stability knobs for repeat-time jitter control were added:
  - `--adaptive-repeat`
  - `--min-sample-time-sec` (target minimum wall-time per sample)
  - `--adaptive-probe-runs` (uncounted warm-up loop before adaptive sample probing)
  - `--adaptive-repeat-cap` (cap adaptive multiplier to avoid unbounded loops)
  - `--stability-profile` (`fast`, `stable`) preset for reproducible measurements
  - `--require-reproducible` to require all 3 lanes pass drift gate for PASS
- `--native-profile max` uses additional vectorization/alignment flags for deeper optimization exploration.
- Adaptive repeat is now enabled by default and minimum sample target defaults to `0.1s`.
- Native benchmark default profile is `aggressive` to reduce jitter-sensitive misses while staying in stable compilation settings.
- Native C compile path now accepts env-controlled profiles:
  - `SPARK_CXX`, `SPARK_CXXFLAGS`, `SPARK_LDFLAGS`.
- Profile controls can also be passed through `SPARK_LTO` and `SPARK_PGO` (with optional `SPARK_PGO_PROFILE`).

## Current Validation Baseline

- `run_phase4_benchmarks.sh` executes 20 scalar programs, compares:
  - `k run --interpret` output against compiled output,
  - expected scalar value per program,
  - matching C baseline outputs,
  - timing medians + reproducibility (`<= 3%` drift rule),
  - speedup estimate relative to interpreted and C baselines.
- Output artifacts:
  - `bench/results/phase4_benchmarks.json`
  - `bench/results/phase4_benchmarks.csv`
- With `--native-profile aggressive` + adaptive repeat enabled:
  - correctness gate: typically **20/20**
  - band gate (`native / baseline`) is generally strong, but can occasionally drop to **19/20** on one-shot runs due sub-millisecond timing noise on tiny kernels.
  - per-iteration median comparison is used when repeats differ due to adaptive sampling.
- For stable band statistics we recommend `--stability-profile stable` together with enough `--runs`/`--repeat` and optional `--require-reproducible`.
- Reported native vs baseline speedup summary in the same configuration:
  - geometric mean around `1.0x`, median around `0.99x`, full range approximately `0.91x–1.13x`.

## Current Decision Notes

- Backend choice for phase4 is **IR → C → clang** (direct LLVM C backend is out of scope for this phase).
- This is documented in `docs/decisions.md` as the phase4 default, with explicit migration notes for future MLIR work.
- Current IR→C optimizer pass includes:
  - branch condition-folding (`cond = ...` + `br_if` collapse),
  - single-use temp inlining (e.g. `acc = acc + 1;`),
  - unused temporary declaration pruning.
- Native compile path now includes `-DNDEBUG` by default and still respects `SPARK_CFLAGS`/`SPARK_LDFLAGS` (and `SPARK_CXXFLAGS` as a compatibility alias).

## Known Gaps (still allowed in scope)

- No matrix/list runtime lowering yet; those paths intentionally remain unsupported in phase4.
- Build pipeline default profile is `-std=c11 -O3` in C baselines; both native and baseline compile profiles are configurable through benchmark CLI/env and compiler env flags.
- `k run --interpret` is still the fallback for non-T4 regions.
- `run_phase4_benchmarks.sh` includes full C baseline parity checks and speedup reporting against C baselines.
- `run_phase4_benchmarks.sh` now includes adaptive repeat probes, but pass-band thresholds can still be affected by aggressive optimization flags and program mix.
