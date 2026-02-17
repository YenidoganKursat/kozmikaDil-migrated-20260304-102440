# Decisions Log

## Decision 1: Build System
- Decision: Use `CMake + Ninja`.
- Why: deterministic generation, clear dependency graph, and easy benchmark target toggling.
- Failure mode if unavailable: fallback to `make` build directory.

## Decision 2: Benchmark Tooling
- Decision: Use a phase-anchored harness with JSON/CSV output and optional `hyperfine`/`perf`.
- Why: objective comparison requires canonical, machine-readable artifacts.
- Fallback: write core artifacts even when optional tools are missing.

## Decision 3: Baseline Language Stack
- Decision: Phase 1 baselines are in C only.
- Why: no language exists yet, so comparisons must be against stable hand-written baselines.

## Decision 4: Correctness Gate
- Decision: each benchmark must emit `pass=PASS|FAIL`, `checksum`, `expected`.
- Why: avoid false positives from perf regressions with wrong output.

## Decision 5: Reproducibility Rule
- Decision: collect >=5 timed runs, trim one min/max, check `max(|ti-median|/median)*100 <= 3.0`.
- Why: controls unstable runs without overfitting to outliers.
- Current phase setting: `RUNS=7` in `bench/scripts/run_phase1_baselines.sh`.

## Decision 6: Memory / Runtime Direction
- Decision: prioritize arena+region style fast paths first, with general GC-style runtime deferred.
- Why: list/matrix-heavy workloads need low-overhead temporary management before broader object features.

## Decision 7: Multi-phase Roadmap Alignment
- Decision: anchor high-risk theoretical ideas (MLIR, Halide, Weld, Triton, etc.) to later phases while keeping Phase 1 purely baseline-focused.
- Why: avoid semantic debt in the earliest phase and keep measurement objective.

## Decision 8: Phase 2 Language Core
- Decision: implement a Python-like expression language with indentation blocks and a minimal evaluator.
- Why: a concrete executable spec enables end-to-end correctness checks before committing to MLIR/codegen.
- Result: parser + AST + interpreter now cover functions, control flow, lists, and basic builtins.
- Decision extension: parser accepts both `[[a,b],[c,d]]` and `[[a,b];[c,d]]` matrix-style input, but normalizes to a single `ListExpr` matrix AST.
- Why: syntax stability matters before compiler lowering; canonical AST keeps backend migration deterministic.

## Decision 9: Value Model
- Decision: keep phase2 `Value` model intentionally small (`nil`, `int`, `double`, `bool`, `list`, `function`, `builtin`).
- Why: constrain runtime surface area, reduce failure modes during phase2, and keep interpreter testable.

## Decision 10: Phase 2 CLI
- Decision: provide a single executable with explicit modes:
  - `sparkc run file.k`
  - `sparkc parse file.k --dump-ast`
- Why: parse/eval paths are separated cleanly; automation and human debugging can use AST output without execution side effects.

## Decision 11: Phase 3 Static Checking Mode
- Decision: add `sparkc check <file.k>` as a separate CLI mode and a dedicated semantic checker for phase3.
- Why: separating parse/typecheck from execution provides deterministic early-failure gates before interpreter/backend work.
- Implementation rule: typecheck should report stable diagnostics for undefined symbols, assignment compatibility, iterable constraints, call arity/type checks, and non-callable invocations.

## Decision 12: Phase 3 Tier Visibility
- Decision: add `sparkc analyze <file.k>` and structured dumps (`--dump-types`, `--dump-shapes`, `--dump-tiers`).
- Why: phase transition criteria for `T4`/`T5`/`T8` classification must be visible and auditable before lowering.
- Implementation rule: default `analyze` emits tier summary; optional flags emit type and shape tables in a line-delimited format.

## Decision 13: Phase 3 Shape/Container Widening Policy
- Decision: model class shape as explicit `open` versus `slots`, and normalize list/matrix widening rules conservatively.
- Why: T4 hedefi için statik kararların geri dönüşümlü, gerekirse normalize edilebilir olması gerekir.
- Implementation rule: mixed container element types map to `Any` and mark as normalizable where analysis can continue via normalization; this drives `T5` diagnostics.

## Decision 14: Empty List and Append Normalization
- Decision: treat `[]` as `List[Unknown]` in phase3 and allow `append` to refine it.
- Why: this enables stable inference (`[] -> List[Int] -> List[Float]` on `append(2.5)`) and explicit `T5` classification for numeric widening scenarios.
- Implementation rule: non-compatible append targets emit `T8`, while numeric widen cases emit `T5` with a normalizable reason when possible.

## Decision 15: Phase 4 Backend Choice
- Decision: use a direct **IR → C → `clang`** backend for phase4 as a bootstrap path.
- Why: deterministic native execution for scalar kernels with minimal extra infra risk in the current milestone.
- Implementation rule: keep phase4 debug commands explicit (`--emit-c`, `--emit-asm`, `--emit-llvm`, `--emit-mlir`) and leave MLIR migration for later phases.

## Decision 16: Compile-Profile Controls for Phase 4
- Decision: expose compiler/linker profile controls through environment variables during phase4 AOT and `run/compile` paths.
- Why: playbook requires heavy build-time optimization options exploration (`LTO`, `PGO`, architecture tuning, link options) without changing command flow.
- Implementation rule: `SPARK_CC`, `SPARK_CFLAGS` (with `SPARK_CXX`/`SPARK_CXXFLAGS` as compatibility aliases), `SPARK_LDFLAGS`, `SPARK_LTO`, and `SPARK_PGO` are honored by `k build`, `k run` (native path), and `k compile --emit-asm`.

## Decision 17: Phase 4 Scalar Canonicalization
- Decision: apply a post-Canonicalization pass in IR→C lowering.
- Why: remove branch-temp and single-use temp artifacts before final C emission to reduce register pressure and simplify branched loops for LLVM.
- Implementation rule: fold `cond + `br_if` into direct conditionals, inline temporaries with single use, and drop dead temporary declarations.

## Decision 18: Adaptive Repeat Benchmarking
- Decision: add warm-up-assisted adaptive repeat probing to phase-4 microbenchmarks (`run_phase4_benchmarks.py`) to stabilize short-kernel timing.
- Why: very fast kernels can produce high timer jitter, causing false reproducibility and speedup regressions.
- Implementation rule:
  - first run a small uncounted probe pass before timing probes,
  - derive per-iteration wall time from measured sample and scale repeat to reach a minimum sample budget,
  - cap adaptive repeats via `--adaptive-repeat-cap` when needed,
  - compare speed with per-iteration medians when repeats differ.

## Decision 19: Default to Stable Phase-4 Measurement Defaults
- Decision: phase-4 benchmarks now default to `--adaptive-repeat` enabled and a `0.1s` minimum sample target.
- Why: consistent timing behavior on fast kernels is more important than minimizing each benchmark run count.
- Implementation rule:
  - `--adaptive-repeat` uses `BooleanOptionalAction` (defaults on),
  - `--min-sample-time-sec` defaults to `0.1`,
  - native benchmark profile defaults to `aggressive`, and profile `max` is available for deeper single-machine tuning.

## Decision 20: Stability Profile and Reproducibility Gate
- Decision: add `--stability-profile` and `--require-reproducible` to phase-4 benchmark harness.
- Why: fast scalar kernels can pass aggregate correctness while still being noisy under strict band comparisons.
- Implementation rule:
  - `--stability-profile=stable` increases minimum sample pressure and activates conservative repeat caps,
  - `--require-reproducible` makes phase pass/fail depend on drift acceptance across interpreted/native/baseline lanes.

## Decision 21: Phase 5 Container Typing Baseline
- Decision: support explicit typing and diagnostics for `append`, `pop`, `insert`, `remove` and matrix elementwise arithmetic in phase 3/type systems before any native container optimizations.
- Why: static typing without these mutators caused false positives and prevented reliable tiering decisions for list/matrix hot paths.
- Implementation rule:
  - `pop` returns element type (or Unknown), while `insert`/`remove`/`append` return Nil.
  - matrix arithmetic allows `+ - * /` for matrix/matrix and matrix/scalar, with shape-check where dimensions are known.
  - `%` is still unsupported for matrix values.

## Decision 22: Phase 5 Matrix Iteration Contract
- Decision: `for row in matrix` binds `row` as `List[T]` (row view/value semantics), not scalar element.
- Why: this matches Python/NumPy mental model and prevents frontend/runtime/codegen semantic drift.
- Implementation rule:
  - semantic checker defines loop variable type as `List[matrix.element]`.
  - codegen lowers matrix iteration by row index and calls `__spark_matrix_row_*`.

## Decision 23: Slice Lowering Coverage in Phase 5
- Decision: lower matrix indexing/slicing combinations directly in codegen:
  - `m[r]`, `m[r,c]`, `m[:,c]`, `m[r0:r1]`, `m[r0:r1, c0:c1]`.
- Why: parser/type-level support without lowering caused false “supported” behavior.
- Implementation rule:
  - use runtime helpers `__spark_matrix_rows_col_*`, `__spark_matrix_slice_rows_*`, `__spark_matrix_slice_block_*`.
  - list slice default stop is `len(list)` when omitted.

## Decision 24: Runtime Call Type Inference Fix
- Decision: `__spark_list_pop_*` is modeled as value-returning in IR->C expression typing (not void).
- Why: void misclassification caused invalid C locals (`void temp`) and forced interpreter fallback.
- Implementation rule:
  - `pop` returns scalar element kind, while `append/insert/remove` remain void mutators.

## Decision 25: Phase 5 Benchmark Profile Defaults
- Decision: benchmark harness now uses phase-aware defaults:
  - phase4: native profile `aggressive`, band `0.9x-1.2x`
  - phase5: native profile `native`, band `0.85x-1.15x`
- Why: phase5 container-heavy workloads on current toolchain were more stable with `native` than `aggressive`, and phase5 acceptance band differs from phase4.
- Implementation rule:
  - explicit CLI flags still override defaults.

## Decision 26: Preserve Call Temps in C Canonicalization
- Decision: temporary assignments whose RHS is a direct function call are not inlined during C canonicalization.
- Why: inlining these temps into loop conditions can duplicate call evaluation and regress performance.
- Implementation rule:
  - only non-call single-use temporaries are inlined.

## Decision 27: Phase 6 Heterogeneous List Semantics
- Decision: `reduce_sum()` and `map_add()` on hetero lists keep container execution valid by applying numeric work on numeric elements and preserving/skipping non-numeric cells based on operation semantics.
- Why: Phase 6 goal is "heterogeneity works" while keeping hot path optimizable via cached plans.
- Implementation rule:
  - `reduce_sum()`: numeric-only accumulation on hetero fallback paths.
  - `map_add()`: numeric elements updated, non-numeric elements preserved in-place order.

## Decision 28: Phase 6 Cache Observability API
- Decision: expose runtime cache diagnostics through container methods:
  - `plan_id()`, `cache_stats()`, `cache_bytes()`.
- Why: Phase 6 requires measurable proof of analyze->plan->materialize->cache behavior and invalidation.
- Implementation rule:
  - mutating list/matrix operations increment cache version and clear materialized buffers,
  - benchmark/test harnesses use these APIs as correctness and steady-state gates.

## Decision 29: Phase 6 Benchmark Stability Control
- Decision: add `sample_repeat` to phase6 benchmark sampling and normalize per-op wall time by effective operation count.
- Why: short-running benchmarks had unstable drift; repeated command execution per sample improved reproducibility without changing semantics.
- Implementation rule:
  - default `sample_repeat=3`,
  - `unit_time_ns = median_sample_time / (ops_per_run * sample_repeat)`.

## Decision 30: Phase 7 Pipeline Fusion Policy
- Decision: introduce a dedicated pipeline chain runtime (`map/filter/zip/reduce/scan`) with fused fast path and explicit non-fused fallback.
- Why: phase7 throughput target depends on removing intermediate containers and minimizing allocation churn.
- Implementation rule:
  - enable fusion by default; allow deterministic fallback with `SPARK_PIPELINE_FUSION=0`,
  - treat random-access heavy / mutating intermediate stages as fusion barriers in perf-tier,
  - expose diagnostics with `--dump-pipeline-ir`, `--dump-fusion-plan`, `--why-not-fused`,
  - benchmark fused vs non-fused and persist JSON/CSV artifacts.

## Decision 31: Phase 7 Receiver Copy Elimination
- Decision: fused pipeline execution now reads receiver containers by const-reference instead of copying whole list/matrix payloads.
- Why: large container copies on every chain invocation were an avoidable steady-state overhead and reduced fusion gains.
- Implementation rule:
  - keep receiver immutable in fused path,
  - only materialize explicit fallback outputs,
  - add PackedInt reduce short-path and small-chain transform dispatch simplification.

## Decision 32: Phase 8 Hybrid Matmul Runtime
- Decision: Phase 8 matmul path uses schedule-driven hybrid backend (`own` tiled kernel + optional BLAS), with pack/cache and epilogue fusion.
- Why: fastest practical delivery is to preserve own-kernel development while allowing BLAS parity path where available.
- Implementation rule:
  - runtime API: `matmul`, `matmul_f32`, `matmul_f64`, `matmul_add`, `matmul_axpby`,
  - schedule controls via env + tuned JSON (`bench/results/matmul_tuned_schedule.json`),
  - expose runtime counters through `matmul_stats()` and `matmul_schedule()`,
  - keep strict numerical defaults (no fast-math by default).

## Decision 33: Phase 8 Schedule Resolution Cache
- Decision: tuned schedule JSON is parsed once per effective config (path/use flag) and reused across matmul calls.
- Why: per-call file read + regex parse in hot path created avoidable overhead and jitter.
- Implementation rule:
  - cache key fields: `SPARK_MATMUL_USE_TUNED`, tuned config path,
  - reload only when these inputs change.

## Decision 34: Phase 8 Fast Matrix Setup Builtins
- Decision: add utility builtins `matrix_fill_affine(...)` and `matmul_expected_sum(lhs, rhs)` to cut interpreter-side setup overhead in matrix benchmarks and scripts.
- Why: phase8 measurements were dominated by interpreted nested fill/expected loops rather than kernel execution.
- Implementation rule:
  - `matrix_fill_affine` emits `Matrix[f64]` with packed cache pre-marked,
  - `matmul_expected_sum` computes checksum reference in runtime C++,
  - phase8 benchmark programs prefer these builtins for stable, kernel-focused timings.
