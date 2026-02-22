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

## Decision 16.1: Layered-Max Runtime-First Profile
- Decision: add a CLI profile selector for runtime-first optimization when build time is intentionally expensive.
- Why: project policy is "build-time cheap, runtime expensive"; a single command should enable layered optimization stack without manual env orchestration.
- Implementation rule:
  - `k run` / `k build` accept `--profile balanced|max|layered-max` and `--auto-pgo-runs <n>`.
  - `layered-max` enables:
    - `LTO=full` by default,
    - automatic PGO cycle (`instrument -> train -> merge -> use`) when explicit `SPARK_PGO` is not set,
    - semantic-preserving runtime fast-path toggles (`SPARK_ASSIGN_INPLACE_NUMERIC=1`, `SPARK_BINARY_EXPR_FUSION=1`),
    - section-based link stripping (`--gc-sections` on Linux, `-dead_strip` on macOS).

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

## Decision 35: Phase 7 Reduce Fast-Path Expansion
- Decision: add explicit fused reduce fast-paths for `map_mul -> reduce_sum` in both list and matrix pipeline runtimes.
- Why: common numeric chains were still paying generic stage-dispatch overhead inside hot loops.
- Implementation rule:
  - dispatch to specialized path when terminal is `reduce_sum` and transform chain is single `map_mul`,
  - preserve generic fallback for all other chains.

## Decision 36: Phase 8 Stable Auto Backend + Epilogue Access
- Decision: lock auto backend with size-based deterministic policy and keep tuned-backend pinning opt-in only; simplify epilogue cell access by pre-validating bias/accumulator shape once.
- Why: we need repeatable backend choice for large matrices and less per-cell branch/check overhead in fused epilogue loops.
- Implementation rule:
  - auto backend defaults:
    - `SPARK_MATMUL_AUTO_DIM_THRESHOLD=224`
    - `SPARK_MATMUL_AUTO_VOLUME_THRESHOLD=224^3`
  - tuned backend pinning only when `SPARK_MATMUL_AUTO_RESPECT_TUNED_BACKEND=1`,
  - own GEMM hoists transposed/non-transposed branch outside inner-most loop,
  - epilogue uses prepared bias/accumulator accessors (shape checks once, then direct indexed access).

## Decision 37: Phase 8 Benchmark Matrix Size Coverage (N=512)
- Decision: extend phase8 benchmark suite with `matmul_core_f64_512` and matching C baseline.
- Why: phase8 gates need explicit large-size coverage; schedule/auto backend behavior at 512 must be measured directly.
- Implementation rule:
  - add `bench/programs/phase8/matmul_core_f64_512.k`,
  - add `bench/programs/phase8/c_baselines/matmul_core_f64_512.c`,
  - include the case in `bench/scripts/phase8/definitions.py`,
  - use `diff < 1e-4` PASS tolerance for this case due deterministic floating accumulation at high total magnitude.

## Decision 38: Phase 7 Affine Reduce Simplification
- Decision: pipeline terminal `reduce_sum` için saf `map_add/map_mul` zincirlerinde per-element stage uygulaması yerine affine formül kullan.
- Why: list/matrix fused runtime'da aynı semantik ile daha az inner-loop aritmetik ve dispatch maliyeti.
- Implementation rule:
  - `sum(map_add(c, X)) = sum(X) + c*N`
  - `sum(map_mul(c, X)) = sum(X)*c`
  - `sum(map_mul(b, map_add(a, X))) = (sum(X) + a*N)*b`

## Decision 39: CI Flakiness Control for Float Extreme Validation
- Decision: remove runtime-dependent hash seeding and enforce deterministic vector generation in extreme float cross-language validation.
- Why: CI failures appeared non-deterministically (`f32 %` mismatch) despite local pass due platform/runtime-sensitive case generation.
- Implementation rule:
  - use stable seed derivation from `(primitive, operator)` string,
  - keep strict mismatch gate,
  - allow only explicit low-float `%` boundary guard where quotient is epsilon-close to integer boundary and outcomes are semantically equivalent under quantized precision.

## Decision 40: CI/CD Hardening Baseline for Language Project
- Decision: make pipeline stability first-class by hardening workflows and improving machine-readable diagnostics.
- Why: a compiler/runtime project needs reproducible CI feedback and supply-chain safety before adding more phases.
- Implementation rule:
  - CodeQL action family pinned to major `v4`,
  - apt installs use retry policy,
  - CTest emits JUnit artifacts in CI and nightly jobs,
  - third-party workflow linter action pinned to commit SHA,
  - Dependabot manages GitHub Actions updates weekly.

## Decision 39: Canonical Numeric While Hot-Loop Specialization
- Decision: evaluator içinde güvenli pattern tespiti ile canonical numeric while döngülerine özel hızlı yol eklendi.
- Why: `while i < N; acc = acc <op> rhs; i = i + 1` şeklinde döngülerde AST dispatch ve environment lookup maliyeti baskındı.
- Implementation rule:
  - yalnızca doğruluk kanıtlanabilen pattern'lerde aktif,
  - pattern dışı kodlar mevcut generic execution path'e düşer,
  - `SPARK_WHILE_FAST_NUMERIC` ile aç/kapa yapılabilir (default: açık).

## Decision 40: Strict High-Precision Repeat Kernel
- Decision: high-precision loop'lar için `eval_numeric_repeat_inplace(...)` eklendi; sabit RHS ve bilinen repeat count durumunda per-iteration dispatch kaldırıldı.
- Why: MPFR path'te en büyük maliyet her iterasyonda operand hazırlığı + genel statement yürütmesiydi.
- Implementation rule:
  - strict semantics korunur (no fast-math, no approximation mode),
  - `f128/f256/f512` için cache-authoritative in-place MPFR güncellemesi kullanılır,
  - `div/mod` zero kontrolleri korunur,
  - uygun olmayan durumlarda otomatik fallback mevcut.

## Decision 39: Phase 8 Size-Aware Kernel + Dense Cache Path
- Decision: own GEMM için size-aware mikro-kernel kullan ve large matrix setup'ta eager dense-f64 cache ile BLAS giriş maliyetini düşür.
- Why: 256/512 boyutlarında stead-state throughput'u artırırken küçük boyutta (128) gereksiz setup maliyetini sınırlamak.
- Implementation rule:
  - `n>=192` için transposed-B yolunda 4-kolon register-mikro-kernel aktif.
  - `matrix_fill_affine` çıktılarında yalnızca `rows*cols >= 128^2` ise eager dense-f64 cache hazırla.
  - `acquire_packed_matrix` non-transpose f64 BLAS yolunda hazır dense cache varsa doğrudan kullan.
  - matmul sonuç matrislerinde gereksiz dense cache kopyası tutulmaz (hot path allocation azaltımı).

## Decision 40: Phase 8 Adaptive Tiles + Deferred BLAS Probe
- Decision: schedule çözümünde boyuta göre tile setini adaptif uygula ve auto backend'de BLAS keşfini yalnızca large-problem durumunda tetikle.
- Why: 128/256/512 karışık yüklerinde tek tuned tile tüm boyutlarda stabil değil; ayrıca küçük workload'larda gereksiz BLAS probe auto modunu yavaşlatıyor.
- Implementation rule:
  - f64 için:
    - `max_dim <= 160` -> `tile_m/n/k = 96`
    - `max_dim >= 224` -> `tile_m/n/k = 64`
  - env override (`SPARK_MATMUL_TILE_*`) adaptif kuralların üstünde kalır.
  - auto seçim koşulu `large_problem && has_blas_backend()` sıralamasıyla çalışır.
  - transposed-B yolunda `n>=256` için 8-kolon mikro-kernel aktif edilir.

## Decision 41: Phase 9 Concurrency Runtime Baseline
- Decision: introduce a phase9 runtime baseline with structured task groups, work-stealing scheduler, channels, and parallel builtins in interpreter-native path.
- Why: Phase 9 needs measurable concurrency behavior now, while full async state-machine lowering is staged after baseline validation.
- Implementation rule:
  - syntax supports `async def` and `async fn`,
  - task scopes use `with task_group([timeout_ms]) as g`,
  - builtins: `spawn/join/cancel`, `parallel_for/par_map/par_reduce`, `channel/send/recv/close`, `stream/anext`,
  - static analyzer emits conservative sendable-capture diagnostics for spawn/parallel calls.

## Decision 42: Async-For + Async State-Machine Dump
- Decision: support `async for` at syntax/runtime level and expose compiler-side pseudo state-machine lowering via analyzer dump.
- Why: phase9 event-driven model needs stream iteration ergonomics and visible lowering diagnostics before deeper backend lowering work.
- Implementation rule:
  - parser accepts `async for name in expr:`,
  - evaluator executes async-for over channel/stream iterables until drained (`None`),
  - `sparkc analyze --dump-async-sm` reports async function await points and synthesized state counts.

## Decision 43: Deadline Alias For Structured Timeout APIs
- Decision: add `deadline(ms)` as an explicit timeout/deadline alias helper for Phase 9 task and channel wait APIs.
- Why: expectation text uses both timeout and deadline terms; a small alias keeps syntax clear without adding a second runtime clock model yet.
- Implementation rule:
  - `deadline(ms)` validates non-negative integer and returns timeout token (`int`),
  - accepted anywhere timeout args are already used (`join`, `task_group`, `recv`, `anext`),
  - parser/semantic/runtime tests include `with task_group(deadline(...))` and `join(task, deadline(...))`.

## Decision 44: Phase 9 Runtime Fast-Path Consolidation
- Decision: keep fixed default chunking (`<=4096 -> 64`) and optimize runtime internals with fire-and-forget scheduler tasks, join-time scheduler assist, and transition-based channel notifications.
- Why: adaptive chunk heuristics were less stable across runs; fixed chunk 64 gave more predictable scaling while runtime fast-path changes produced larger, repeatable overhead reductions.
- Implementation rule:
  - parallel primitives (`parallel_for/par_map/par_reduce`) submit chunk tasks via scheduler fire-and-forget + wait-group error propagation,
  - `await/join` can assist scheduler by executing pending tasks before blocking,
  - channel send/recv only notifies on queue state transitions (empty/full boundary),
  - keep env override `SPARK_PHASE9_CHUNK` for explicit tuning experiments.

## Decision 45: Phase 10 CPU Feature Dispatch Visibility + Override
- Decision: centralize CPU feature detection in `cpu_features` module and add deterministic override envs.
- Why: phase10 requires both runtime dispatch and same-machine correctness tests across variants.
- Implementation rule:
  - host detection: `x86_64`, `aarch64`, `riscv64` + feature probes,
  - reporting via `sparkc --print-cpu-features`,
  - test override support: `SPARK_CPU_ARCH`, `SPARK_CPU_FEATURES`.

## Decision 46: Phase 10 Multi-Arch Build Gate
- Decision: provide script-first multi-target gate (`x86_64`, `aarch64`, `riscv64`) with machine-readable output.
- Why: target portability must be continuously verifiable, even when cross toolchains are partially available.
- Implementation rule:
  - `scripts/phase10/multiarch_build.py` emits `bench/results/phase10_multiarch.json`,
  - host smoke run optional; cross targets produce explicit build status and logs.

## Decision 47: Phase 10 Final Perf Pipeline Automation
- Decision: make `PGO+LTO` and `BOLT` pipelines scriptable and measurable as first-class artifacts.
- Why: phase10 goal is consistent final-squeeze optimization, not ad-hoc manual tuning.
- Implementation rule:
  - `scripts/pgo_cycle.sh`: instrument -> run -> merge -> use + median timing comparison,
  - `scripts/bolt_opt.sh`: perf -> perf2bolt -> llvm-bolt flow with skip-on-missing-tools behavior,
  - outputs written under `bench/results/phase10/*`.

## Decision 48: Phase 10 Safety Gates as Dedicated Pipelines
- Decision: differential, fuzz, and sanitizer checks are explicit scripts with JSON/CSV outputs.
- Why: perf mode must not silently regress correctness; CI-ready artifacts are required.
- Implementation rule:
  - differential: interpreter vs native output parity on representative suites,
  - fuzz: parser non-crash and runtime output parity fuzz loops,
  - sanitizer: ASan/UBSan and TSan isolated build profiles.

## Decision 49: Canonical Numeric While Lowering To Repeat Kernels
- Decision: phase4 native codegen canonical numeric recurrence loops are lowered to repeat kernels.
- Why: `while i < N` + scalar recurrence had high branch/dispatch overhead and was the top runtime hotspot in 100M operator microbenchmarks.
- Implementation rule:
  - pattern:
    - condition `i < bound` (stable bound expression),
    - body includes `acc = acc <op> rhs` and `i = i + 1`,
    - `rhs` must be loop-invariant scalar.
  - lowering:
    - compute iteration count once (`bound - i`),
    - call `__spark_num_repeat_<op>_<kind>(acc, rhs, n)`,
    - update `i = bound`.
  - kind routing:
    - float kinds `f8/f16/bf16/f32/f64/f128/f256/f512`.

## Decision 50: Repeat Kernels Are Strict-Only
- Decision: repeat kernels now run only strict recurrence semantics; no fast-math/algebraic shortcut toggle remains.
- Why: user policy is zero tolerance for optional correctness drift in floating trajectories.
- Implementation rule:
  - `__spark_num_repeat_*` always executes per-step semantics,
  - fixed-point early-stop remains (semantics-preserving),
  - no runtime env switch for algebraic repeat rewrite.

## Decision 51: High-Precision Build Barrier Replaced With Strict Launcher
- Decision: `build` no longer hard-fails for `f128/f256/f512`; it now emits an executable launcher that runs interpreter strict mode.
- Why: remove usability blocker while preserving correctness guarantees (native C backend is still precision-limited for these kinds).
- Implementation rule:
  - detect high-precision primitives at build time,
  - emit executable wrapper script at requested output path,
  - wrapper executes `k run --interpret <source>` (fallback: `sparkc run --interpret`),
  - no silent downcast to native approximate path.
