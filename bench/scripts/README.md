# Benchmark Script Naming and Responsibilities

Rule: one script = one primary task.

## Naming
- `benchmark_*`: runs and measures one benchmark scenario.
- `generate_*`: generates benchmark source artifacts only.
- `run_*`: compatibility wrapper or shell launcher only.
- `*_runtime`: wall-time runtime measurement flow.

## Current Cross-Language Tasks
- `benchmark_crosslang_matmul_pair_runtime.py`: benchmark `A * B` matrix multiply scenario.
- `benchmark_crosslang_matmul_chain4_runtime.py`: benchmark `((A * B) * C) * D` chain scenario.
- `benchmark_crosslang_generic_ops_fastinit.py`: benchmark generic list/matrix ops fast-init scenario.

Launchers:
- `benchmark_crosslang_matmul_pair_runtime.sh`
- `benchmark_crosslang_matmul_chain4_runtime.sh`
- `benchmark_crosslang_generic_ops_fastinit.sh`

## Source Generation Helpers
- `crosslang/generate_matmul_pair_sources.py`: generates sources for pair-matmul benchmark.
- `crosslang/generate_matmul_chain4_sources.py`: generates sources for chain4 benchmark.
- `crosslang/benchmark_core.py`: shared measurement/parsing dataclasses and helpers.

## Primitive Benchmarks
- `primitives/benchmark_primitive_scalar_core.py`: core runner for one primitive.
- `primitives/benchmark_<primitive>_scalar_runtime.py`: one wrapper per primitive (`i8..i512`, `f8..f512`).
- `primitives/benchmark_all_primitives_cycle.py`: baseline (1x) and current speed cycle across all primitives.
  - supports `--mode interpret|native|auto` for controlled baseline vs optimized runs.
- `primitives/benchmark_mixed_signed_ops_100m.py`: mixed-sign operator runtime benchmark entrypoint.
  - internals split by task under `primitives/mixed_ops/`:
    - `constants.py`: primitive/operator sets.
    - `programs.py`: benchmark source generation.
    - `runners.py`: backend-specific execution logic.
    - `io_utils.py`: result persistence.
    - `cli.py`: orchestration and argument flow.

Compatibility wrappers exist to avoid breaking old commands; wrappers must only delegate.
