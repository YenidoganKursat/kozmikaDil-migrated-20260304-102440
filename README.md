# Codex Spark (Phase 1)

## Quick Start (Ubuntu)

```bash
bash scripts/ubuntu_toolchain.sh
bash scripts/bootstrap_phase1.sh
```

## Repo Structure

- `compiler/` 
- `runtime/`
- `stdlib/`
- `tests/`
- `bench/`
- `docs/`

Architecture docs:
- `docs/architecture/repo_layers.md`
- `docs/architecture/dev_native_workflow.md`

## Available Phase 1 Commands

```bash
cmake -S . -B .build_phase1 -G Ninja -DSPARK_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build .build_phase1
bash bench/scripts/run_phase1_baselines.sh
```

`bench/scripts/run_phase1_baselines.sh` creates:
- `bench/results/phase1_raw.json`
- `bench/results/phase1_raw.csv`
- `bench/results/hyperfine.json` (if `hyperfine` is installed)
- `bench/results/perf_stats.txt` (if `perf` is installed)

## Phase 2 Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./k parse examples/main.k --dump-ast
./k run examples/main.k
```

## Phase 3 Commands

```bash
./k analyze examples/main.k
./k analyze examples/main.k --dump-types
./k analyze examples/main.k --dump-shapes
./k analyze examples/main.k --dump-tiers
```

## Phase 4 Commands

```bash
./k compile examples/main.k
./k compile --emit-c examples/main.k
./k compile --emit-c --emit-c-out build/main.cpp examples/main.k
./k run examples/main.k
./k run --interpret examples/main.k
./k build examples/main.k -o build/main.bin
./k build examples/main.k -o build/main.max.bin --profile layered-max --auto-pgo-runs 3
./k run examples/main.k --profile layered-max --auto-pgo-runs 2
./bench/scripts/run_phase4_benchmarks.sh
./bench/scripts/run_phase4_benchmarks.sh --runs 7 --warmup-runs 2 --native-cflags "-O3 -march=native -flto"
./bench/scripts/run_phase4_benchmarks.sh --native-lto thin --native-pgo instrument --native-profile aggressive
./bench/scripts/run_phase4_benchmarks.sh --runs 7 --warmup-runs 2 --repeat 4
./bench/scripts/run_phase4_benchmarks.sh --runs 5 --warmup-runs 1 --repeat 8 --adaptive-repeat --adaptive-probe-runs 1 --adaptive-repeat-cap 64
./bench/scripts/run_phase4_benchmarks.sh --runs 7 --warmup-runs 1 --repeat 8 --adaptive-repeat --adaptive-probe-runs 1 --adaptive-repeat-cap 24 --native-profile aggressive
./bench/scripts/run_phase4_benchmarks.sh --runs 7 --warmup-runs 1 --repeat 8 --adaptive-repeat --adaptive-repeat-cap 24 --native-profile max
./bench/scripts/run_phase4_benchmarks.sh --stability-profile stable --runs 9 --repeat 4 --min-sample-time-sec 0.2 --adaptive-repeat-cap 64
```

Phase 4 benchmark defaults are now set to:
- `--adaptive-repeat` enabled
- `--native-profile aggressive`

Current default run on a supported desktop machine is expected to produce:
- `phase4 benchmark pass (with C baseline band): 19/20` can happen on one-shot runs due very short kernels and jitter; use `--runs`/`--stability-profile` to stabilize.
- `native vs baseline speedup`: geometric mean ≈ `1.0x`, median ≈ `0.99x`
- `native vs interpreted speedup`: geometric mean often around `~44x`

Faz 4’de ayrıca:
- `k analyze file.k --dump-tiers` T4/T5/T8 görünürlüğünü verir.
- `k compile --emit-asm|--emit-llvm|--emit-mlir` IR/assembly çıktısını sağlar.
- `./bench/scripts/run_phase4_benchmarks.sh` faz4 için `k run --interpret`, `k build`-native ve C baseline karşılaştırmalı ölçümü üretir (`bench/results/phase4_benchmarks.json`, `.csv`).
- `./bench/scripts/run_phase4_benchmarks.sh` default profiler `--native-profile aggressive` kullanır.
- Bench marker script supports tuning profiles:
  - `--native-cxx`, `--baseline-cxx`
  - `--native-cflags`, `--baseline-cflags`
- `--native-profile`, `--baseline-profile` (`portable`/`native`/`aggressive`/`max`) for deeper optimization experiments.
  - `--native-lto`, `--baseline-lto` (`off`/`thin`/`full`)
  - `--native-pgo`, `--baseline-pgo` (`off`/`instrument`/`use`)
  - `--pgo-profile` (required when `--*-pgo=use`)
  - `--warmup-runs`
  - `--repeat` (run each sample N times and sum wall-time)
    (defaults to `SPARK_PHASE4_REPEAT` or `8`).
- `--adaptive-repeat` is enabled by default and `--adaptive-probe-runs` defaults to `1`.
- `--stability-profile fast|stable` tunes benchmark run parameters for deterministic measurement.
- `--require-reproducible` adds a strict reproducibility gate to `pass`.
- Native compiler profile can also be driven directly with:
  - `SPARK_CXX`, `SPARK_CXXFLAGS`, `SPARK_LDFLAGS`.
- `k run/build` için yeni katmanlı profil:
  - `--profile balanced|max|layered-max`
  - `--auto-pgo-runs <n>` (`layered-max` ile auto-PGO eğitim turu sayısı)
  - `layered-max`: build-time pahalı, runtime odaklı (`LTO=full`, auto-PGO, in-place numeric assign, binary expression fusion).

Tests:

```bash
cmake -S . -B build -DSPARK_BUILD_TESTS=ON
cmake --build build -j4
./build/compiler/sparkc_smoke_test
./build/compiler/sparkc_parser_tests
./build/compiler/sparkc_eval_tests
./build/compiler/sparkc_typecheck_tests
./build/compiler/sparkc_codegen_tests
./build/compiler/sparkc_phase5_tests
./build/compiler/sparkc_phase6_tests
./build/compiler/sparkc_phase7_tests
./build/compiler/sparkc_phase8_tests
./build/compiler/sparkc_phase9_tests
./build/compiler/sparkc_phase10_tests
```

Execution modes:
- `interpret`: correctness/debug path (`k run --interpret ...`)
- `native`: compiled runtime path (`k build ... && ./binary`)
- `builtin` benchmark helpers: micro-kernel measurement path only

## Phase 7 Commands

```bash
./k analyze examples/main.k --dump-pipeline-ir
./k analyze examples/main.k --dump-fusion-plan
./k analyze examples/main.k --why-not-fused
./bench/scripts/run_phase7_benchmarks.sh
```

## Phase 8 Commands

```bash
./k run --interpret bench/programs/phase8/matmul_core_f64.k
./k run --interpret bench/programs/phase8/matmul_epilogue_f64.k
./bench/scripts/run_phase8_benchmarks.sh
python3 tune/matmul_tuner.py
# Runtime-only fair cross-language compare (build excluded):
./bench/scripts/run_crosslang_matmul_runtime.sh --n 100 --repeats 100 --runs 5 --warmup 1
```

## Phase 9 Commands

```bash
./k analyze bench/programs/phase9/spawn_join_overhead.k --dump-async-sm
./bench/scripts/run_phase9_benchmarks.sh
```

## Phase 10 Commands

```bash
./k --print-cpu-features
./k build bench/programs/phase4/scalar_sum.k --target aarch64-linux-gnu -o build/scalar.aarch64.bin
./k analyze bench/programs/phase6/hetero_promote_reduce_steady.k --dump-layout

# Multi-arch and final perf pipeline
./scripts/phase10_multiarch.sh --run-host-smoke
./scripts/pgo_cycle.sh --program bench/programs/phase10/pgo_call_chain_large.k --lto thin
./scripts/bolt_opt.sh --binary bench/results/phase10/pgo/native_pgo.bin --profile-cmd bench/results/phase10/pgo/native_pgo.bin
./bench/scripts/run_phase10_benchmarks.sh

# Correctness/safety gates
./scripts/phase10_safety_gates.sh

# Release artifact
./scripts/release_package.sh 0.10.0-rc1
```
