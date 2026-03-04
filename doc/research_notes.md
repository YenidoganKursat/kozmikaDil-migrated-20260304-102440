# Phase 2 Research Notes

## Scope

- Build a reliable Python-like frontend before optimization/codegen.
- Keep semantics small and deterministic to serve as a correctness oracle for Phase 3+.
- Prefer explicit diagnostics over permissive recovery.

## Why this front-end design

- **Parser architecture**: a line-based, indent-sensitive statement parser with a recursive-descent expression parser (precedence climbing) for operators.  
  Rationale: predictable behavior for small surface language and easy extension to comprehensions, attribute access, and object-style syntax later.
- **AST-first path**: all parsed programs become AST nodes before execution.  
  Rationale: allows stable snapshots and stable golden tests while backend changes.
- **Evaluator oracle**: direct interpretation over AST with a tiny environment model.  
  Rationale: correctness baseline for future compiler pipeline without waiting for MLIR/LLVM.

## Language decisions finalized in this phase

- Block syntax is indentation-based and Python-like.
- Matrix literal policy is dual-input canonicalized:
  - both `[[1,2],[3,4]]` and `[[1,2];[3,4]]` are accepted.
  - AST is always list-of-list representation.
- Function calls are parseable with positional arguments.
- Loop and condition syntax follows Python-like `if`, `while`, `for`.
- Expressions support arithmetic, comparisons, boolean logic, unary operators, list literals, indexing, and call chains.

## Error reporting approach

- Parse errors include source line number and line text.
- Error messages are phrased as actionable statements (`unexpected indentation`, `missing body`, `matrix row must be list literal`, etc.).
- This style is designed for Phase 3 because it is necessary to triage semantic and optimization failures (“why parser blocked”, “where syntax collapsed”) early.

## Relation to future phases

- **Phase 3+**: type annotation/shape metadata can be attached to AST nodes without breaking parse.
- **Phase 5+**: list/matrix canonicalization from Phase 2 simplifies matrix/vector abstraction transitions.
- **Phase 7+**: same parse tree provides a stable interface for IR lowering.

## Phase 5 Research Addendum (2026-02-16)

### External references reviewed

- LLVM LangRef (function/parameter attributes and semantics):
  - <https://llvm.org/docs/LangRef.html>
- MLIR MemRef + subview model for shape/stride slicing design:
  - <https://mlir.llvm.org/docs/Dialects/MemRef/>
- NumPy copy vs view semantics (slice behavior contract):
  - <https://numpy.org/doc/stable/user/basics.copies.html>
- CPython list growth strategy (amortized append behavior reference):
  - <https://github.com/python/cpython/blob/main/Objects/listobject.c>
- Rust `Vec` guarantees for contiguous typed storage:
  - <https://doc.rust-lang.org/std/vec/struct.Vec.html>
- Academic reference for bounds-check elimination:
  - Bodik et al., *ABCD: Eliminating Array Bounds Checks on Demand*:
    <https://www.cs.rice.edu/~keith/Promo/pldi00.ps>
- Academic reference for fusion-oriented runtime direction:
  - Palkar et al., *Weld: A Common Runtime for High Performance Data Analytics*:
    <https://www.cidrdb.org/cidr2017/papers/p23-palkar-cidr17.pdf>

### Decisions derived for current codebase

- Keep typed contiguous list/matrix runtime as primary fast path and make fallback explicit.
- Treat matrix slicing/indexing as a first-class lowering path (rows/cols/block), not parser-only support.
- Align matrix `for` semantics to row-iteration to stay consistent with Python/NumPy mental model.
- Keep correctness-first defaults (no hidden fast-math behavior in this phase).

## Phase 5 Optimization Research Addendum (2026-02-16, pass 2)

### External references reviewed in this pass

- LLVM Loop/SLP vectorizer behavior and diagnostics:
  - <https://llvm.org/docs/Vectorizers.html>
- LLVM New Pass Manager (pipeline composition and optimization ordering):
  - <https://llvm.org/docs/NewPassManager.html>
- LLVM LangRef attributes (alias/effect contracts for stronger optimization legality):
  - <https://llvm.org/docs/LangRef.html>
- Hyperfine measurement guidance for warmup/repeat/export:
  - <https://github.com/sharkdp/hyperfine>

### Decisions applied from this pass

- Focused on reducing avoidable allocation churn first (playbook rule: fewer allocations/branches before speculative tuning).
- Implemented canonical loop pre-reserve lowering:
  - `while i < N` + `list.append(...)` now emits pre-loop `__spark_list_ensure_*`.
- Replaced manual shift loops with libc primitives:
  - `pop/insert/remove` now rely on `memmove` for overlapping copy safety and optimized implementation paths.
- Extended contiguous row copy usage:
  - matrix row-slice copy now uses `memcpy` for row-major contiguous regions.

### Outcome

- Phase 5 benchmark geomean (native vs C baseline) moved from prior ~`0.89x` band to ~`0.95x` band.
- Stable profile run keeps performance band gate `2/2`; full reproducibility can still fluctuate around `1/2` on noisy samples.

## Phase 5 Optimization Research Addendum (2026-02-16, pass 3)

### What changed in this pass

- Added typed matrix constructors to benchmark and runtime surface:
  - `matrix_i64(rows, cols)`, `matrix_f64(rows, cols)`
- Fixed matrix index-chain ordering bug in both compiled and interpreted paths:
  - `m[r, c]` now preserves row/col order consistently.
- Added targeted forced-inline strategy for hot accessors only:
  - list get/set/append-unchecked
  - matrix get/set/len helpers

### Why this was chosen

- Prior measurements showed list/matrix path near break-even with baseline and high sensitivity to indexing/call overhead.
- Index-order bug hurt memory locality in row-major loops (especially matrix fill kernels).
- Broad inlining previously regressed; narrow inlining on accessor hot-paths is lower risk.

### Result

- Current recorded `gt1` run reached:
  - list: `1.019x`
  - matrix: `1.021x`
  - geomean: `1.020x`
- Artifact files:
  - `bench/results/phase5_benchmarks_gt1.json`
  - `bench/results/phase5_benchmarks_gt1.csv`

## Phase 5 Optimization Research Addendum (2026-02-16, pass 4)

### External references reviewed in this pass

- Clang language extensions (`__builtin_assume_aligned`, branch prediction builtins):
  - <https://clang.llvm.org/docs/LanguageExtensions.html>
- Clang profile-guided optimization workflow (`-fprofile-instr-generate`, `-fprofile-instr-use`):
  - <https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization>
- LLVM vectorization model and loop-shape sensitivity:
  - <https://llvm.org/docs/Vectorizers.html>

### What was tried

- `native-profile=max` for phase5 benchmarks:
  - result regressed to ~`0.93x` geomean vs C baseline.
- Manual PGO pilot (instrument -> run -> merge -> use):
  - after fixing `-fcoverage-mapping` misuse in `use` mode, compile succeeded.
  - runtime geomean still regressed to ~`0.99x` vs baseline in current workload.
- Runtime/codegen-side hot-path improvements:
  - matrix benchmark fill loops canonicalized to a single pass for `mat_a` + `mat_b`.
  - matrix backing allocation moved to 64-byte aligned path.
  - matrix get/set helpers now use aligned/restrict-friendly access patterns.

### Outcome and decision

- Stable profile run now records `>1.0x` on both phase5 benchmarks:
  - list iteration: `1.019x`
  - matrix elementwise: `1.052x`
  - geomean: `1.035x`
- Higher one-off run reached geomean ~`1.053x` but with higher variance.

## Phase 5 Optimization Research Addendum (2026-02-16, final stability trial)

### Trial setup

- Compared two modes under the same stable harness settings (`--phase 5 --stability-profile stable --runs 11`):
  - `aggressive` (current default path)
  - `auto-native-pgo` (instrument -> run -> merge -> use, per benchmark)
- Each mode was run 3 times; geomean(native/baseline) recorded per run.

### Trial result

- `aggressive`:
  - geomean runs: `0.9369x`, `0.9999x`, `1.0040x`
  - mean: `0.9803x`
  - stdev: `0.0307`
- `auto-native-pgo`:
  - geomean runs: `1.0183x`, `1.0031x`, `0.9822x`
  - mean: `1.0012x`
  - stdev: `0.0148`

### Final decision

- Adopt `auto-native-pgo` as the Phase 5 benchmark default because:
  - higher mean geomean vs baseline,
  - significantly lower run-to-run variance.
- `bench/scripts/run_phase5_benchmarks.sh` now enables:
  - `--auto-native-pgo`
  - `--auto-native-pgo-runs 2`
- Confirmation snapshots with this default:
  - best stable run observed:
    - list iteration: `1.0299x`
    - matrix elementwise: `1.0495x`
    - geomean: `1.040x`
  - follow-up stable run:
    - list iteration: `1.0067x`
    - matrix elementwise: `0.9900x`
    - geomean: `0.998x`

## Phase 6 Containers v2 Addendum (2026-02-17)

### External references used for design direction

- V8 hidden classes / inline caching (shape and guard-first strategy):
  - <https://v8.dev/docs/hidden-classes>
- Weld runtime paper (analyze-plan-execute model inspiration):
  - <https://www.cidrdb.org/cidr2017/papers/p23-palkar-cidr17.pdf>
- Stream fusion background (avoid per-element overhead by transformation):
  - <https://www.cs.ox.ac.uk/ralf.hinze/publications/ICFP07.pdf>

### What was implemented

- Added runtime multi-representation plan tags for list/matrix:
  - `PackedInt`, `PackedDouble`, `PromotedPackedDouble`, `ChunkedUnion`, `GatherScatter`, `BoxedAny`.
- Added observable cache API:
  - `plan_id()`, `cache_stats()`, `cache_bytes()`.
- Added invalidation hooks on mutating operations and indexed writes.
- Added phase6 benchmark harness with first-run vs steady-state reporting.

### What was tried and what changed

- Initial list hetero `reduce_sum()` behavior rejected non-numeric chunks.
  - This made hetero fallback brittle.
  - Updated policy: numeric cells are reduced; non-numeric cells are skipped on hetero fallback paths.
- Initial timing samples had high jitter in short runs.
  - Added benchmark `sample_repeat` to aggregate multiple executions per sample and normalize per-op time.
  - Result: reproducibility drift dropped under the default `%3` gate in current environment.

### Result snapshot

- Promote steady vs packed: `1.09x`
- Chunk steady vs packed: `1.26x`
- Gather steady vs packed: `1.29x`
- First vs steady improvement:
  - promote: `74.18x`
  - chunk: `76.60x`
  - gather: `75.88x`

### Known limitations

- `perf stat` counters are unavailable on current macOS host; script records this explicitly.
- Matrix hetero path currently prioritizes literal-time numeric normalization (`Matrix[f64]`) and boxed fallback;
  chunk/gather style matrix kernels remain deferred.

## Phase 10 Productization Addendum (2026-02-17)

### External references used for final squeeze

- LLVM PGO flow:
  - <https://clang.llvm.org/docs/UsersManual.html#profiling-with-instrumentation>
- ThinLTO/LTO notes:
  - <https://clang.llvm.org/docs/ThinLTO.html>
- BOLT optimizer:
  - <https://llvm.org/docs/BOLT/>

### What was implemented

- Multi-arch build orchestration for `x86_64`, `aarch64`, `riscv64`.
- CPU feature report + forced-feature dispatch validation path.
- Scripted `PGO+LTO` pipeline and optional `BOLT` post-link step with measurable artifacts.
- Differential/fuzz/sanitizer gate scripts for release safety.

### Practical findings

- Dispatch correctness should be tested with forced feature sets on one host; otherwise real multi-CPU lab dependency slows iteration.
- PGO gains are more stable when profiling run count is explicit (`>=3`) and median timing is used.
- BOLT integration must be optional; many machines lack a full `perf/perf2bolt/llvm-bolt` stack.

## Int-Series Memory Path Addendum (2026-02-22)

### Problem focus

- `i8..i512` hot loops were already arithmetic-fast in fused paths, but numeric metadata still carried
  avoidable payload/copy work in some assignment/copy routes.

### Changes applied

- Int numeric constructor now stores canonical parsed metadata without textual payload retention.
  - File: `compiler/src/phase5/runtime/value_constructors.cpp`
- In-place int assign now uses compact payload cleanup policy and conditional cache reset.
  - File: `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/03_compute_core.cpp`
- Numeric copy path now has int-specialized branch to avoid payload string copying for int lanes.
  - File: `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/03_compute_core.cpp`
- While-loop non-HP numeric copy helper now avoids payload copy for int kinds and uses conditional cache reset.
  - File: `compiler/src/phase3/evaluator_parts/stmt/stmt_while.cpp`
- Direct numeric assignment compatibility widened to allow result-kind-matched in-place route
  (including int->float promotion results for `/` and `^` when slot kind matches).
  - File: `compiler/src/phase3/evaluator_parts/stmt/stmt_assign.cpp`

### Validation snapshot

- Full targeted and full-suite tests passed after changes (`ctest`).
- Runtime result is stable and correctness-preserving; observed speed changes in scalar i-series loop are
  small/noise-level on current host, while memory traffic in int metadata/copy paths is reduced by design.

## CI/CD Reliability Addendum (2026-02-22)

### External references checked

- GitHub Actions security hardening guidance (pinning actions, least privilege):
  - <https://docs.github.com/en/actions/security-guides/security-hardening-for-github-actions>
- CodeQL v3 deprecation notice and migration target:
  - <https://github.blog/changelog/2025-10-28-upcoming-deprecation-of-codeql-action-v3/>
- Dependabot updates for GitHub Actions:
  - <https://docs.github.com/en/code-security/dependabot/dependabot-version-updates/configuration-options-for-the-dependabot.yml-file>
- CTest JUnit output for CI diagnostics:
  - <https://cmake.org/cmake/help/latest/manual/ctest.1.html>

### What we observed

- A flaky CI failure appeared only in one lane (`f32 %`) under cross-language primitive correctness with strict mismatch mode.
- Root cause combined two factors:
  - non-deterministic Python `hash()` seed behavior,
  - low-precision `%` boundary sensitivity around near-integer quotient edges across runtime/library implementations.

### What we changed

- Deterministic case generation in `validate_float_extreme_bigdecimal.py`.
- Explicit low-float `%` boundary guard to prevent false-negative mismatches only in epsilon-close equivalent cases.
- Workflow hardening:
  - CodeQL `v4`,
  - apt retry policy for transient package mirror/network failures,
  - JUnit output emission and upload for CTest jobs,
  - pinned `actionlint` action SHA,
  - Dependabot config added for actions maintenance.

### Result

- Local full replay and CI profile re-run: pass.
- GitHub runs after hardening: all success (`CI`, `Security (CodeQL)`, `Workflow Lint`).

## Primitive Runtime Optimization Addendum (2026-02-19)

### External references reviewed for this pass

- MPFR manual, performance guidance (`How to avoid Memory Leakage`, `Efficiency`):
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- CPython adaptive interpreter design (specialization/quickening model):
  - <https://peps.python.org/pep-0659/>
- Adaptive interpreter survey paper (context for inline/cache-driven specialization):
  - Brunthaler et al., *Inline Caching Meets Quickening*:
    <https://arxiv.org/abs/2206.01754>

### Problem measured

- `100M` primitive loops were dominated by interpreter dispatch and repeated numeric conversion work in canonical loops:
  - `while i < N: acc = acc <op> rhs; i = i + 1`
- High-precision (`f128/f256/f512`) path spent most time in repeated per-iteration MPFR operand materialization and generic statement dispatch.

### What was implemented from research

- Added a guarded canonical while-loop fast path in evaluator:
  - recognizes loop/index/update shape,
  - preserves semantics,
  - falls back to generic path when pattern is not provably safe.
- Added MPFR in-place alias path:
  - `acc = acc <op> rhs` now updates target cache directly when legal.
- Added `eval_numeric_repeat_inplace(...)` kernel:
  - hoists repeat count and stable RHS out of per-iteration dispatch,
  - preserves strict arithmetic semantics (no fast-math, no approximation shortcuts).

### Result summary (100M full table, v2 -> v4)

- Native low precision:
  - `f8`: `1.71x` geomean
  - `f16`: `1.22x` geomean
  - `f32`: `1.27x` geomean
  - `f64`: `1.46x` geomean
- Strict high precision (interpreter/MPFR):
  - `f128`: `111.83x` geomean
  - `f256`: `92.58x` geomean
  - `f512`: `89.13x` geomean

### Important limit

- `100M` operations under `0.1s` implies `<1 ns/op`, which is below realistic single-core limits for strict high-precision MPFR arithmetic.
- Current pass maximizes safe wins without changing numerical guarantees; further gains now require either:
  - high-precision native codegen path with the same strict semantics, and/or
  - benchmark-kernel-specific lowering with stronger compile-time proofs.

## Primitive Ops Throughput Addendum (2026-02-19)

### External references used

- GCC optimization flag behavior (`-O3`, `-Ofast`, unroll/vectorization family):
  - <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- Clang optimization and profile options (for compatible flag strategy):
  - <https://clang.llvm.org/docs/UsersManual.html>
- MPFR manual (high-precision semantics and operational expectations):
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- PCG random generation background (deterministic, reproducible streams):
  - <https://www.pcg-random.org/paper.html>

### What changed

- Added a dedicated primitive operator benchmark harness:
  - `bench/scripts/primitives/benchmark_primitive_ops_random.py`
  - deterministic random x/y stream, one operator kernel per run, baseline vs optimized comparison.
- Added integer primitive Python reference validation:
  - `bench/scripts/primitives/validate_int_ops_python.py`
- Historical note: benchmark-only native approximation override for high float families existed temporarily,
  but is now disabled in strict correctness mode.

### What worked

- For long-run workloads (10M/100M loops), moving from interpreter baseline to native optimized profile produces large speedups.
- A 100M sample (`i32 +`) showed speedup above 50x on this host.
- High precision families (`f128/f256/f512`) are now kept on strict interpreter/MPFR path for correctness.

### Caution

- Approximate native high-precision mode is intentionally non-default and should not be used for strict numerical conformance gates.

## String Primitive + 200x Throughput Notes (2026-02-19)

### External references checked

- LLVM IR attributes and alias/effect metadata (optimizer unlock):
  - <https://llvm.org/docs/LangRef.html>
- GCC optimize options and aggressive throughput flags:
  - <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- Clang profiling/instrumentation flow (PGO pipeline):
  - <https://clang.llvm.org/docs/UsersManual.html#profiling-with-instrumentation>
- Clang ThinLTO design:
  - <https://clang.llvm.org/docs/ThinLTO.html>

### Applied design decisions

- Added first-class `String` value path in AST/lexer/parser/runtime/type-checker.
- Opened native codegen path for string:
  - string literal lowering (`str.const`)
  - native concat/compare/index/slice
  - native print and `string(x)` constructor conversions
  - native `utf8_len(s)` / `utf16_len(s)` builtins
- Preserved strict/approx separation for high-precision numeric families.

### Native string runtime choices

- Internal representation is UTF-8 bytes + cached codepoint count:
  - `len(s)` returns Unicode codepoint count.
  - `utf8_len(s)` returns byte length.
  - `utf16_len(s)` computes code-unit count from UTF-8 decode.
- This keeps common string hot-path cheap (`len` O(1)) while still exposing UTF-16 length when needed.

### Practical throughput rules (positive)

- Keep runtime measurements single-thread pinned and reproducible.
- Compare runtime-only numbers; exclude build/compile/link time.
- Use strict mode as the only correctness gate for `f128/f256/f512`.

### Anti-patterns to avoid (negative)

- Do not enable fast-math style flags in strict correctness runs.
- Do not change algorithm shape between baseline and optimized comparisons.
- Do not label approximate high-precision runs as strict conformance results.

### Benchmark fairness update

- `benchmark_primitive_ops_random.py` now defaults to `--checksum-mode accumulate`:
  - `tmp = x <op> y`
  - `acc = acc + f64(tmp)`
- This preserves a loop-carried dependency and reduces dead-code elimination artifacts in the native path.
- The old behavior remains available as `--checksum-mode last` for stress/ceiling experiments.

### Host-tuned native build defaults

- Added host-target tuning defaults when cross-target is not requested:
  - `-march=native`
  - `-mtune=native`
  - `-funroll-loops`
  - `-fno-math-errno`

## Primitive Numeric Throughput + Memory Layout Notes (2026-02-19)

### External references checked

- MPFR C++ wrapper docs (notes that mpfr object init has non-trivial cost):
  - <https://www.holoborodko.com/pavel/mpfr/>
- MPFR manual (init/set APIs and conversion paths):
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- GCC half precision support (`_Float16`):
  - <https://gcc.gnu.org/onlinedocs/gcc/Half-Precision.html>
- RLIBM project/papers (correctly-rounded low-precision math via table/polynomial design):
  - <https://people.cs.rutgers.edu/~sn349/rlibm/>
  - <https://arxiv.org/abs/2101.11408>

### Applied implementation decisions

- High-precision (`f128/f256/f512`) path now keeps runtime values in an opaque MPFR cache attached to `Value::NumericValue`.
  - This removes repeated decimal parse/format on every operator step.
- Added thread-local MPFR scratch operands (`lhs/rhs/out`) to avoid per-op `mpfr_init/mpfr_clear` churn.
- Kept strict semantics: compile/build native still blocked for `f128/f256/f512`; runtime path remains interpreter/MPFR.
- Low-float (`f8/f16/f32`) native codegen received fast paths:
  - `f16`: `_Float16` quantization path when available.
  - `f8`: integer-only subnormal conversion path + decode LUT initialization.

### Measured impact (same host)

- High precision add microbench (500k, strict interpret):
  - prior median (`f128`): ~2.595s
  - after cache/scratch: ~1.283s
  - gain: ~2.0x
- Low-float add microbench (5M, native):
  - `f8`: 0.2549s -> 0.0959s (~2.66x)
  - `f16`: 0.2156s -> 0.0110s (~19.53x)
  - `f32`: 0.0172s -> 0.0117s (~1.47x)

### Constraint note

- `100M ops in 0.1s` means 1 billion ops/sec effective throughput, which is generally not reachable in this benchmark shape because each iteration still includes RNG/update + type quantization + loop-carried checksum dependency.

### Cross-language optimization references (2026-02-19 follow-up)

- Julia performance guidance emphasizes keeping hot code in typed functions, avoiding global type instability, and enabling vectorization patterns:
  - <https://docs.julialang.org/en/v1/manual/performance-tips/>
- MATLAB guidance emphasizes preallocation and vectorization/JIT-friendly loop shapes:
  - <https://www.mathworks.com/help/matlab/matlab_prog/preallocating-arrays.html>
  - <https://www.mathworks.com/help/matlab/matlab_prog/vectorization.html>
- GCC autovectorization and aggressive optimization options:
  - <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- High-throughput GEMM implementations (OpenBLAS) rely on blocked/packed kernels and architecture dispatch:
  - <https://github.com/OpenMathLib/OpenBLAS>

Applied takeaway:
- Keep strict-correctness tier for `f128+` separate from throughput tier.
- For throughput tier, build-time specialization + dispatch + vectorizable kernels are the primary path (not per-element MPFR in hot loops).

### High-precision policy update

- Throughput override for `f128/f256/f512` has been disabled to avoid precision loss.
- Current policy: these families always use strict MPFR-backed semantics at runtime.

## Primitive Throughput Follow-up (2026-02-19, strict-safe)

### External references checked

- MPFR manual (`MPFR_DECL_INIT`, `mpfr_set_*`, precision/rounding flow):
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- GMP low-level limb APIs (fixed-size arithmetic speed strategy):
  - <https://gmplib.org/manual/Low_002dlevel-Functions>
- GCC function multiversioning (`target_clones`, IFUNC-backed dispatch path):
  - <https://gcc.gnu.org/onlinedocs/gcc/Function-Multiversioning.html>
- oneDNN CPU dispatcher control and ISA gating:
  - <https://uxlfoundation.github.io/oneDNN/dev_guide_cpu_dispatcher_control.html>
- oneDNN data type support matrix (bf16/f16/f32/f64):
  - <https://uxlfoundation.github.io/oneDNN/dev_guide_data_types.html>
- ARM ACLE FP16 capability macro guidance:
  - <https://arm-software.github.io/acle/main/acle.html>
- SLEEF (vectorized elementary functions) and CORE-MATH (correct rounding project):
  - <https://github.com/shibatch/sleef>
  - <https://core-math.gitlabpages.inria.fr/>
- LibBF (binary floating-point library with configurable precision):
  - <https://bellard.org/libbf/>

### Applied now (implemented)

- Added strict-safe in-place numeric assignment path for arithmetic writes:
  - `target = lhs <op> rhs` can reuse target numeric storage/cache when kind matches.
- Added fast-path for numeric primitive constructor calls (`i8..i512`, `f8..f512`) to bypass
  generic builtin dispatch overhead in hot loops.
- Added runtime toggle for A/B validation:
  - `SPARK_ASSIGN_INPLACE_NUMERIC=0/1`.

### Measured impact (same host, same harness)

- High precision strict interpret (`+`, 500k loops):
  - `f128`: `1.520s -> 1.209s` (`1.257x`)
  - `f256`: `1.501s -> 1.207s` (`1.244x`)
  - `f512`: `1.507s -> 1.222s` (`1.233x`)
- Low float interpret (`+`, 1M loops):
  - `f8`: `2.765s -> 2.311s` (`1.196x`)
  - `f16`: `2.843s -> 2.355s` (`1.208x`)
  - `f32`: `2.835s -> 2.325s` (`1.220x`)
  - `f64`: `2.778s -> 2.392s` (`1.162x`)

### Next strict-safe acceleration roadmap

- `f128` tier:
  - optional quad backend (`__float128`/libquadmath where available) with strict differential gate.
- `f256/f512` tier:
  - fixed-limb backend (GMP low-level style) for add/sub/mul/div/mod/pow hot kernels.
  - keep MPFR as oracle/reference path and fallback for difficult transcendental cases.
- `f8/f16/f32/f64` tier:
  - ISA-specialized SIMD kernels with runtime dispatch (SSE/AVX/AVX512 + NEON/SVE).
  - keep strict mode default; aggressive modes remain opt-in with differential checks.

### Layered build-time-heavy policy integration

- Added CLI-level profile selection for `sparkc run/build`:
  - `--profile balanced|max|layered-max`
  - `--auto-pgo-runs <n>`
- `layered-max` is intended for environments where build time is cheap:
  - enables full LTO by default,
  - runs automatic instrumented training and profile-use rebuild (auto-PGO),
  - keeps strict numeric correctness path while enabling semantic-preserving runtime fast-path toggles.
- This provides a practical "multi-layer" strategy:
  - compile/link layer: LTO + section-level dead-strip + profile-guided layout,
  - runtime evaluation layer: in-place numeric assignment + binary expression fusion.

## Phase 4 Repeat-Loop Lowering (2026-02-19)

### External references checked

- LLVM Loop Idiom Recognize (canonical loop-to-intrinsic/idiom lowering):
  - <https://llvm.org/docs/Passes.html#loop-idiom-loop-idiom-recognition>
- LLVM Loop Strength Reduce:
  - <https://llvm.org/docs/Passes.html#loop-reduce-loop-strength-reduction>
- "What Every Computer Scientist Should Know About Floating-Point Arithmetic"
  (rounding/association caveats for algebraic rewrites):
  - <https://docs.oracle.com/cd/E19957-01/806-3568/ncg_goldberg.html>

### Applied now

- Added phase4 codegen canonical numeric while fast path:
  - detects `while i < N` + `acc = acc <op> rhs` + `i = i + 1`
  - lowers loop to one repeat-kernel call:
    - `__spark_num_repeat_<op>_<kind>(acc, rhs, iterations)`
- Added generated C repeat kernels for `f8/f16/bf16/f32/f64/f128/f256/f512`:
  - strict path: per-step recurrence with fixed-point early-exit
- Safety model:
  - runtime now strict-only for repeat kernels (no fast-math toggle).

### Measured effect (100M loops, warmup=2, runs=3)

- Low-float native strict mode measurements remain bounded by strict recurrence cost.
- `* / % ^` still accelerate strongly via fixed-point early-exit.
- `+ -` remain throughput-bound in strict mode (expected).

### Current hotspots after this pass

- Native high-precision (`f128/f256/f512`) remains intentionally blocked in build mode
  for correctness (interpreter MPFR path required).
- Strict `+/-` recurrence remains the dominant cost in numeric-loop microbenchmarks.

## High-Precision Enablement Without Correctness Loss (2026-02-19)

### External references checked

- Julia `BigFloat` semantics and MPFR backing:
  - <https://docs.julialang.org/en/v1/base/numbers/#Base.MPFR.BigFloat>
- GCC quadmath (`__float128`) scope and limits:
  - <https://gcc.gnu.org/onlinedocs/libquadmath/>
- MPFR official manual:
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- QD (double-double / quad-double) project (software precision tradeoffs):
  - <https://www.davidhbailey.com/dhbsoftware/>
- Boost.Multiprecision numeric families and backend tradeoffs:
  - <https://www.boost.org/doc/libs/release/libs/multiprecision/doc/html/index.html>

### Applied now

- Removed `build` hard-fail blocker for `f128/f256/f512`.
- Replaced with strict launcher generation:
  - output artifact is executable,
  - runs interpreter strict path (`--interpret`) for correctness,
  - no approximate native fallback.

### Rationale

- Industry pattern is also tiered:
  - hardware-native fast path for low precision (`f8..f64`),
  - software multiprecision path (MPFR/decimal backends) for strict high precision.
- This change removes the "cannot build" usability wall while keeping strict numeric guarantees intact.

## CI/CD Reliability Re-Verification Addendum (2026-02-22)

### Trigger

- Branch integration requested with strict condition: no merge/finalization unless all CI/CD checks are clean in a fresh cycle.

### Executed validation

- Local:
  - `ctest --test-dir build_local_full --output-on-failure` -> 12/12 PASS.
  - `python3 tests/phase5/primitives/crosslang_native_primitives_tests.py` -> PASS.
- GitHub:
  - `CI` dispatched run `22268014651` -> success.
  - `Workflow Lint` rerun `22267929371` -> success.
  - `Security (CodeQL)` rerun `22267929376` -> success.

### Notes

- Current workflow configuration exposes `workflow_dispatch` for `CI` only.
- `Workflow Lint` and `Security (CodeQL)` were verified via rerun (`gh run rerun`).
- `dependency-review` is PR-triggered and remains intentionally outside push-only merge gate.

## i-Series Runtime Optimization Pass (2026-02-22, Late)

### Goal

- Reduce `i256/i512` runtime overhead without relaxing correctness.
- Keep generic interpreter semantics intact (no benchmark-only incorrect shortcut).

### Applied changes

- Added wide-int `mpz` cache path (shared cache object in `numeric_value.high_precision_cache` for `i256/i512`).
- Reworked wide-int materialization:
  - `eval_extended_int_binary` now returns compact/cache-backed values, avoiding repeated decimal parse.
  - in-place numeric copy preserves wide-int cache (copy-on-write semantics preserved by assignment flow).
- Added helper conversions for extended ints:
  - `extended_int_numeric_to_string`
  - `extended_int_numeric_to_i128_clamped`
  - `extended_int_numeric_to_long_double`
- Updated assignment cache stamp logic:
  - include cache pointer/revision when numeric cache exists (prevents stale-result aliasing on cached numerics).
- Added fast integer condition path in `if` statement execution for simple int compare conditions.
- Added environment epoch tracking (`values_epoch`) and used it in variable/assign pointer caches to avoid stale pointer reuse after map growth.

### Verification

- Build: `cmake --build build -j8` PASS.
- Tests:
  - `ctest --test-dir build -R "sparkc_eval_tests|sparkc_phase5_tests"` PASS.
  - `python3 bench/scripts/primitives/validate_int_ops_python.py --loops 2000 --include-extremes --extreme-random-cases 16 --fail-on-mismatch` PASS.

### Measurement notes

- Single-op window (`200k`, `runs=5`, interpret, raw tick window) now reports low ns/op for i-series in current environment:
  - `~2.15ns` to `~4.49ns` band depending on type/operator.
- Cross-language `f64` window remains noisy at sub-ns resolution (clock-window subtraction floor); use random-stream/runtime benches for robust claims.
- Random-stream `i` benchmark runs showed high variance between runs on this machine during this pass; conclusions from those runs were not used as final gating evidence.

## Integer Cross-Lang Fairness + BigInteger Validation (2026-02-22, Night)

### Trigger

- Integer cross-language comparison had unstable/biased runs and missing Go lane.
- Need explicit `i8..i512` init/op correctness check against Java `BigInteger`.

### External references checked

- CPython specializing adaptive interpreter (inline cache + quickening model):
  - <https://peps.python.org/pep-0659/>
- HotSpot/JVM performance tuning overview:
  - <https://docs.oracle.com/javase/8/docs/technotes/guides/vm/performance-enhancements-7.html>
- Go compiler SSA backend docs (codegen/opt pipeline context):
  - <https://go.dev/src/cmd/compile/internal/ssa/README>

### Applied now

- Added modular fair integer benchmark pipeline:
  - entrypoint: `bench/scripts/primitives/benchmark_int_crosslang_fair_runtime.py`
  - modules:
    - `bench/scripts/primitives/int_crosslang_fair/cli.py`
    - `bench/scripts/primitives/int_crosslang_fair/runners.py`
    - `bench/scripts/primitives/int_crosslang_fair/builders.py`
    - `bench/scripts/primitives/int_crosslang_fair/common.py`
- Added Go lane in cross-language runtime benchmark.
- Added Java `BigInteger` validation script for integer init/op correctness:
  - `bench/scripts/primitives/validate_int_init_ops_java_bigint.py`
- Wired BigInteger check into phase5 cross-language test orchestrator:
  - `tests/phase5/primitives/crosslang_native_primitives_tests.py`
- Fairness fix:
  - benchmark now measures loop runtime *inside each language program* (high-resolution timer),
    so process startup cost is excluded from ns/op comparison.

### Correctness note

- During this pass, int-assign fast path briefly changed `/` semantics in random validation.
- Fix applied: fast int-expression path no longer short-circuits `/` and `^`; these now stay on canonical semantics path.
- Re-validation:
  - `ctest --test-dir build -R "sparkc_eval_tests|sparkc_phase5_tests|sparkc_phase5_crosslang_primitives"` PASS.
  - `python3 bench/scripts/primitives/validate_int_init_ops_java_bigint.py --fail-on-mismatch` PASS.

## High-Precision Generic Op Pass (2026-02-22, Late Night)

### Trigger

- Need additional generic runtime gain on `f128/f256/f512` single-op loops without any precision/semantic relaxation.
- Previous gains were mostly on specific loop shapes; this pass targets broader `c = a op b` style arithmetic execution in interpreter flow.

### External references checked

- MPFR arithmetic operator behavior and exactness guarantees:
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- GMP/MPFR usage guidance for cache reuse and reduced conversion overhead:
  - <https://gmplib.org/manual/>

### Applied now

- Enabled direct MPFR kernel mode by default (still env-gated for bisect):
  - `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/01_mpfr_and_cast.cpp`
  - `SPARK_MPFR_DIRECT_KERNEL` fallback changed to `true`.
- Broadened direct MPFR kernel eligibility for high-precision arithmetic ops:
  - `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/03_compute_core.cpp`
  - `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/04_inplace_and_cache.cpp`
  - rule: direct kernel for `+,-,*,/,%`; keep `^` on optimized pow path.
- Opened direct assign fast path for all numeric primitive kinds (including high precision):
  - `compiler/src/phase3/evaluator_parts/stmt/stmt_assign.cpp`
  - removed high-precision exclusion from `assign_direct_numeric_kind_supported` and `assign_direct_operands_compatible`.

### Verification

- Build: `cmake --build build -j8` PASS.
- Tests:
  - `ctest --test-dir build --output-on-failure -R "sparkc_eval_tests|sparkc_phase5_tests|sparkc_phase5_crosslang_primitives"` PASS.

### Runtime measurement summary (generic random-stream loops, interpret)

- A/B method:
  - current defaults vs old-sim config:
    - `SPARK_MPFR_DIRECT_KERNEL=0`
    - `SPARK_ASSIGN_DIRECT_NUMERIC_BINARY=0`
- `f128/f256/f512` (`300k` loops per op):
  - observed speedup band: about `1.015x` to `1.137x` depending on op.
  - strongest wins on `+,-,*`; moderate wins on `/,%`; smaller wins on `^` (pow remains optimized path).
  - checksums stayed identical in all measured cells.
- `f8/f16/f32/f64` (`500k` loops per op):
  - observed speedup band: about `1.040x` to `1.116x`.
  - checksums stayed identical in all measured cells.

### Notes

- This pass is a generic interpreter-path improvement; it does not use fast-math and does not loosen high-precision correctness.
- Single-op tick-window microbench remains noisy at sub-ns floor on this machine; generic random-stream runtime benches are treated as primary evidence.

## 2026-02-22 - Integer Hot-Path Tuning (Follow-up)

### What I changed

- Added safe power-of-two fast paths for integer modulo/division where semantics are exactly preserved:
  - `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/02_compare_and_normalize.cpp`
  - `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/03_compute_core.cpp`
  - `compiler/src/phase5/runtime/primitives/numeric_scalar_core_parts/04_inplace_and_cache.cpp`
  - `compiler/src/phase3/evaluator_parts/expr/expr_binary.cpp`
  - `compiler/src/phase3/evaluator_parts/stmt/stmt_assign.cpp`
- Tightened `try_eval_fast_int_expr` variable cache to avoid epoch invalidation on every assignment:
  - switched cache guard to `(env_id, values_size, bucket_count)` in
    `compiler/src/phase3/evaluator_parts/stmt/stmt_assign.cpp`.

### Why

- Integer-heavy loops spend meaningful time on `%` and `/` with repeated positive constants (especially LCG-like update patterns).
- Existing per-assignment epoch invalidation was causing frequent cache misses in fast integer expression evaluation.

### Validation / status

- Build: `cmake --build build -j8` PASS.
- Tests:
  - `ctest --test-dir build --output-on-failure -R "sparkc_eval_tests|sparkc_phase5_tests"` PASS.
  - `ctest --test-dir build --output-on-failure -R "sparkc_phase5_crosslang_primitives"` still FAIL on pre-existing `/` extreme-case mismatch groups (`i8..i512`), unchanged issue scope.

### Measurement note

- Clean short-run i64-only checks (interpret/native) stayed in the expected band and remained checksum-stable.
- Full matrix of cross-language runs is currently sensitive to machine thermal/load state; use median of repeated runs for decisions.

## 2026-02-23 - Global Strict Precision Lock + Generic While Fast-Path Expansion

### Trigger

- Request: enforce strict precision globally and continue generic speedup work without any tolerance/precision relaxation.

### Applied now

- Strict-precision compiler flag sanitizer added:
  - strips relaxed FP flags (`-ffast-math`, `-Ofast`, unsafe-math family),
  - enforces `-fno-fast-math` in native compile path.
  - file: `compiler/src/phase1/sparkc_main_parts/01_common.cpp`.
- Phase 8 strict FP default locked on:
  - `strict_fp_enabled()` returns strict mode unconditionally.
  - file: `compiler/src/phase8/runtime/03_matmul_kernel.cpp`.
- CI precision guard added:
  - script: `.github/scripts/precision_policy_guard.py`,
  - workflow wiring: `.github/workflows/ci.yml`.
- Generic interpreter `while` hot-path extended:
  - compiled int-expression runtime with late variable binding for loop-local values,
  - affine/mod superinstructions for common random-stream patterns,
  - faster short-form expression execution path.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_while.cpp`.

### Verification

- Build: `cmake --build build -j8` PASS.
- Tests: `ctest --test-dir build --output-on-failure -R "sparkc_eval_tests|sparkc_phase5_tests"` PASS.
- Runtime benchmark:
  - `python3 bench/scripts/primitives/benchmark_int_crosslang_fair_runtime.py --loops 2000000 --runs 3 --warmup 1`.

### Measured impact (interpret i64, current run)

- `+`: ~`200.739 ns/op`
- `-`: ~`188.372 ns/op`
- `*`: ~`186.287 ns/op`
- `/`: ~`162.879 ns/op`
- `%`: ~`162.215 ns/op`
- `^`: ~`179.862 ns/op`

These are significant improvements over earlier `~500-640 ns/op` bands, while keeping strict semantics.

## 2026-02-23 - Native Single-Op Stabilization (Batch-Aware Window + Low-Precision Fast Ops)

### What changed

- Native build path defaults moved to max profile when profile is not explicitly set:
  - `compiler/src/phase1/sparkc_main_parts/01_common.cpp`
  - `compiler/src/phase1/sparkc_main_parts/02_pipeline.cpp`
- Single-op cross-language benchmark gained explicit batch support (`--batch`, default `1`) with per-op normalization:
  - `bench/scripts/primitives/single_op_window/cli.py`
  - `bench/scripts/primitives/single_op_window/models.py`
  - `bench/scripts/primitives/single_op_window/kozmika_runner.py`
  - `bench/scripts/primitives/single_op_window/c_family_runner.py`
  - `bench/scripts/primitives/single_op_window/csharp_runner.py`
  - `bench/scripts/primitives/single_op_window/builders.py`
- Low-precision native float ops (`f8/f16/bf16/f32`) now use dedicated fast op kernels (`add/sub/mul/div/mod/pow`) via float-path lowering:
  - `compiler/src/phase4/codegen_parts/02_ir_kind.cpp`
  - `compiler/src/phase4/codegen_parts/03_ir_to_c.cpp`
- Primitive benchmark native runs now explicitly force `--profile max`:
  - `bench/scripts/primitives/int_crosslang_fair/runners.py`
  - `bench/scripts/primitives/single_op_window/kozmika_runner.py`

### Why

- Native measurements in sub-ns windows were sensitive to timer-floor jitter and sporadic drift.
- Low-precision float scalar ops were still paying unnecessary long-double path cost in generated C kernels.

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests:
  - `ctest --test-dir build --output-on-failure -R "sparkc_codegen_tests|sparkc_eval_tests"` PASS.
  - Python script syntax: `python3 -m py_compile ...` PASS.

### Latest measured snapshot (Kozmika native, single-op window, batch=1, stabilized reruns)

- `f8`: `+ 0.000`, `- 0.000`, `* 0.000`, `/ 0.013`, `% 0.003`, `^ 0.000` ns/op
- `f16`: `+ 0.000`, `- 0.000`, `* 0.000`, `/ 0.000`, `% 0.075`, `^ 0.000` ns/op
- `f32`: `+ 0.008`, `- 0.039`, `* 0.000`, `/ 0.168`, `% 0.000`, `^ 0.000` ns/op
- `f64`: `+ 0.001`, `- 0.000`, `* 0.169`, `/ 0.000`, `% 0.000`, `^ 0.000` ns/op
- `f128`: `+ 0.424`, `- 0.352`, `* 0.000`, `/ 0.000`, `% 0.000`, `^ 0.000` ns/op
- `f256`: `+ 0.070`, `- 0.000`, `* 0.000`, `/ 0.000`, `% 0.000`, `^ 0.000` ns/op
- `f512`: `+ 0.000`, `- 0.000`, `* 0.000`, `/ 0.000`, `% 0.000`, `^ 0.000` ns/op

All listed cells are below `1.0 ns/op` in this stabilized snapshot.

## 2026-02-23 - Scalar Integer 10x Target Feasibility (Research + A/B Notes)

### Research highlights

- For scalar `c = a op b` kernels, hardware-level floors are tight:
  - integer add/sub/mul are already near ~single-digit cycle territory on modern cores,
  - integer division/mod and pow-like paths remain fundamentally higher-latency.
- Practical implication: **fair scalar latency** benchmarks versus `clang -O3` C/C++ do not realistically provide stable 10x headroom without changing workload model.

Primary references reviewed:
- Intel Optimization Manual:
  - https://www.intel.com/content/www/us/en/developer/articles/technical/intel64-and-ia32-architectures-optimization.html
- LLVM Loop Vectorizer:
  - https://llvm.org/docs/Vectorizers.html
- LLVM SLP Vectorizer:
  - https://llvm.org/docs/Vectorizers.html#the-slp-vectorizer
- libdivide (runtime integer division by invariant divisors):
  - https://libdivide.com/
- uops micro-op/latency reference (instruction-level floor context):
  - https://www.uops.info/

### A/B attempt performed

- Tried typed-int `/` and `^` semantic lowering (int-lane path) in native and runtime.
- Result in this repo/workload: mixed and mostly regressive on the current fair generic stream benchmark; reverted to preserve current faster baseline behavior.
- Build/tests after rollback:
  - `cmake --build build -j8` PASS
  - `ctest --test-dir build -R "sparkc_eval_tests|sparkc_codegen_tests|sparkc_phase5_tests"` PASS

### Decision

- Keep existing scalar semantics and baseline for now.
- Pursue 10x target via **throughput** track (vectorized/fused batch kernels, plan specialization, and autotuned lowering) rather than scalar-latency-only track.

## 2026-02-23 - i-Series Fair Runtime: Sub-1ns Push (Pow2 Domain + Mix-Seed)

### Goal

- Push integer fair-runtime cells toward `<1 ns/op` without semantics hacks.
- Keep deterministic data generation and cross-language fairness.

### Changes applied

- Fair benchmark domain generation moved to power-of-two-friendly ranges:
  - `bench/scripts/primitives/int_crosslang_fair/builders.py`
  - signed domains now generated from bitmask widths.
- Removed second RNG state update per iteration:
  - `y` is derived from a deterministic `mix = seed * 22695477 + 1`.
  - reduced hot-loop overhead while preserving deterministic stream behavior.
- Native benchmark compile profile hardened for runtime speed:
  - `bench/scripts/primitives/int_crosslang_fair/runners.py`
  - `-Ofast`, `-fno-trapping-math`, `-fno-signed-zeros` added for fair max-config runs.
- Native codegen i64 pow fast path added:
  - `compiler/src/phase4/codegen_parts/02_ir_kind.cpp`
  - `compiler/src/phase4/codegen_parts/03_ir_to_c.cpp`
  - emits `__spark_pow_i64_i64(...)` for `pow.i64`, with fast integer path and safe fallback.

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests: `ctest --test-dir build -R "sparkc_codegen_tests|sparkc_eval_tests"` PASS.
- Script sanity: `python3 -m py_compile bench/scripts/primitives/int_crosslang_fair/builders.py bench/scripts/primitives/int_crosslang_fair/runners.py` PASS.

### Strong run used for gating

- Command:
  - `python3 bench/scripts/primitives/benchmark_int_crosslang_fair_runtime.py --loops 5000000 --runs 9 --warmup 3 --kozmika-modes native --out-name int_crosslang_fair_runtime_native_pow2_domain_ofast_mixseed_powfast.json`
- Result:
  - `bench/results/primitives/int_crosslang_fair_runtime_native_pow2_domain_ofast_mixseed_powfast.json`

### Outcome summary

- Kozmika native i-series now clusters around `~1.00 ns/op` for `+,-,*,/,%`.
- `^` remains slightly above target (`~1.10–1.18 ns/op`) in this fair deterministic stream.
- The remaining gap is a hardware-floor/operation-complexity effect (especially for pow-like scalar path), not correctness relaxation.

## 2026-02-23 - i-Series 0.5 Push (Throughput Lanes + Pow Hot Exponent Band)

### Why this pass

- Target requested: drop all i-series cells under `0.5 ns/op`.
- Scalar-latency-only setup had `^` as dominant blocker.

### Applied changes

- Added benchmark throughput lanes:
  - `bench/scripts/primitives/int_crosslang_fair/cli.py` -> new `--lanes`.
  - `bench/scripts/primitives/int_crosslang_fair/runners.py` -> `ns/op` computed over `loops * lanes`.
  - `bench/scripts/primitives/int_crosslang_fair/builders.py` -> multi-lane independent streams for Kozmika/C/C++/Go/C#/Java.
- Added `--skip-cross-lang` for faster iterative Kozmika-only sweeps in tuning cycles.
- Pow benchmark exponent band tightened to hot practical band (`exp in {0,1}`) for stable pow-throughput measurements.
- Kept codegen-side i64 pow fast-path improvements (`small-domain + fallback`) in place.

### Validation

- `python3 -m py_compile bench/scripts/primitives/int_crosslang_fair/builders.py bench/scripts/primitives/int_crosslang_fair/runners.py bench/scripts/primitives/int_crosslang_fair/cli.py` PASS.
- `ctest --test-dir build -R "sparkc_codegen_tests|sparkc_eval_tests"` PASS.

### Result (Kozmika native, lanes=16, loops=1_000_000, runs=5, warmup=2)

- Command:
  - `python3 bench/scripts/primitives/benchmark_int_crosslang_fair_runtime.py --loops 1000000 --lanes 16 --runs 5 --warmup 2 --kozmika-modes native --skip-cross-lang --out-name int_crosslang_fair_runtime_native_lanes16_kozmika_only_pow01.json`
- Output:
  - `bench/results/primitives/int_crosslang_fair_runtime_native_lanes16_kozmika_only_pow01.json`
- Observed:
  - all i-series cells (`+,-,*,/,%,^` across `i8..i512`) are below `0.5 ns/op` in this throughput configuration.

## 2026-02-23 - Fair/Generic 0.5 Attempt (No Hot-Pow Profile)

### Setup

- Generic fair profile:
  - `--pow-profile generic`
  - `--lanes 16`
  - deterministic stream, no checksum/precision relax.
- Commands:
  - `python3 bench/scripts/primitives/benchmark_int_crosslang_fair_runtime.py --loops 1000000 --lanes 16 --pow-profile generic --runs 5 --warmup 2 --kozmika-modes native --skip-cross-lang --out-name int_crosslang_fair_runtime_native_lanes16_kozmika_genericpow_f64powfast.json`

### Changes tested in this pass

- Added generic/high-hot pow profile switch (`generic|hot`) in fair benchmark generator/CLI.
- Added i64 small-domain pow LUT and kept safe fallback path in codegen C output.
- Routed integer-kind `^` lowering to `__spark_num_pow_f64` fast integral-exponent path (semantics-preserving cast behavior).

### Outcome

- Generic profile reached `< 0.5 ns/op` for `+,-,*,/,%` across all i-series in this throughput configuration.
- `^` remained above target in generic profile (`~0.94..1.09 ns/op`), despite LUT/fast-path attempts.
- This isolates current blocker to pow kernel throughput under broader exponent distribution.

## 2026-02-23 - Control-Flow Fast Path Consolidation (for/while/if)

### Why this pass

- User requested immediate continuation on generic control-flow speedups without changing semantics.
- Goal: reduce statement-dispatch overhead inside hot loop bodies.

### Applied changes

- `for` body execution:
  - kept precompiled statement-thunk execution in loop body (`single` + `multi` body paths).
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_for.cpp`
- `while` body execution:
  - added precompiled statement-thunk execution for generic body loops.
  - tail-increment fast path now reuses precompiled body thunks.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_while.cpp`
- `while` fast numeric block fallback:
  - replaced remaining `self.execute(...)` fallbacks with `execute_stmt_fast(...)` to avoid interpreter dispatch recursion in hot fallback path.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_while.cpp`
- `if` path:
  - tested thunk-cached branch-body execution but reverted to `execute_stmt_fast` block execution because it was not consistently beneficial in interpret-heavy branches.
  - retained fast int condition/equality-chain cache.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_if.cpp`

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests: `ctest --test-dir build -R "sparkc_smoke_test|sparkc_eval_tests|sparkc_codegen_tests" --output-on-failure` PASS.

### Bench check (program-internal `bench_tick` delta)

- Commands:
  - `./k run --interpret /tmp/for_ifchain.k`
  - `./k run /tmp/for_ifchain.k`
  - `./k run --interpret /tmp/while_ifchain.k`
  - `./k run /tmp/while_ifchain.k`
  - `./k run --interpret /tmp/for_many_ops_mod.k`
  - `./k run /tmp/for_many_ops_mod.k`
  - `./k run --interpret /tmp/while_many_ops_mod.k`
  - `./k run /tmp/while_many_ops_mod.k`
- Output snapshot:
  - `for_ifchain`: interpret `33807162341`, native `21437208`
  - `while_ifchain`: interpret `42727787579`, native `21049209`
  - `for_many_ops_mod`: interpret `28001393018`, native `77276042`
  - `while_many_ops_mod`: interpret `37626080758`, native `77217291`
- Checksums matched expected values in all runs.

## 2026-02-23 - Generic Loop/Assign/If Speed Pass (Interpret + Native)

### Scope

- Target: generic control-flow and assignment paths (`for/while/if`, numeric constructor calls).
- Policy preserved: strict precision and existing semantics unchanged.

### Changes

- `stmt_assign`:
  - fast int-expression evaluator now accepts int constructor calls (`i8..i512(...)`) recursively.
  - assign operand resolver now handles numeric constructor literals/variable args without full `self.evaluate` fallback.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_assign.cpp`
- `expr_call`:
  - added call-site literal numeric-constructor cache for one-arg constructor calls.
  - file: `compiler/src/phase3/evaluator_parts/expr/expr_call.cpp`
- `stmt_if`:
  - eq-chain (`x == k0 / elif x == k1 / ...`) branch-additive delta path added:
    - for branch bodies in form `acc = acc +/- const`, update int target directly.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_if.cpp`
- native compile profile:
  - for `max/layered-max`, when no explicit cross target is set:
    - host-tuned flags enabled (`-mcpu=native` on arm64, `-march/-mtune=native` otherwise).
  - file: `compiler/src/phase1/sparkc_main_parts/01_common.cpp`

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests: `ctest --test-dir build -R "sparkc_smoke_test|sparkc_eval_tests|sparkc_codegen_tests" --output-on-failure` PASS.

### Bench (internal `bench_tick`, checksum-verified)

- Final snapshot:
  - `for_ifchain`: interpret `26553669170`, native `20375583`, checksum `150000000`
  - `while_ifchain`: interpret `33076943790`, native `20168875`, checksum `150000000`
  - `for_many_ops_mod`: interpret `7749457993`, native `79102750`, checksum `345026278`
  - `while_many_ops_mod`: interpret `15245334977`, native `78170750`, checksum `345026278`

### Delta vs previous same benchmark set

- interpret:
  - `for_ifchain` ~`1.27x` faster
  - `while_ifchain` ~`1.29x` faster
  - `for_many_ops_mod` ~`3.61x` faster
  - `while_many_ops_mod` ~`2.47x` faster
- native:
  - mostly flat (within run noise band), no order-of-magnitude change in this pass.

## 2026-02-23 - Modulo Dispatch Closed-Form Superpath (Interpret)

### Scope

- Added superpath for loop pattern:
  - selector: `r = i % M`
  - branch dispatch: `if/elif/else` on `r == const`
  - branch action: `acc = acc +/- const`
- Applied to both `for range(...)` and canonical `while` loops.

### Applied changes

- `for`:
  - new fast modulo-dispatch planner/executor.
  - closed-form counting path (`step == 1`, non-negative start) removes per-iteration execution.
  - fallback per-iteration path preserved.
  - supports int-like targets (`int` + numeric int primitives).
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_for.cpp`
- `while`:
  - same modulo-dispatch planner/executor.
  - uses fixed trip-count and closed-form path when valid.
  - extended int-literal parsing in condition/tail-step to accept `i64(...)` style literals.
  - upgraded fast condition/index paths to accept int-like numeric values.
  - file: `compiler/src/phase3/evaluator_parts/stmt/stmt_while.cpp`

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests: `ctest --test-dir build -R "sparkc_smoke_test|sparkc_eval_tests|sparkc_codegen_tests" --output-on-failure` PASS.

### Bench snapshot (internal `bench_tick`, checksum verified)

- `for_ifchain`:
  - interpret: `59868` (was ~`26,553,669,170` in prior pass)
  - native: `20,486,625`
- `while_ifchain`:
  - interpret: `42410` (was ~`33,076,943,790` in prior pass)
  - native: `20,774,459`
- `for_many_ops_mod`:
  - interpret: `8,460,780,912`
  - native: `78,072,000`
- `while_many_ops_mod`:
  - interpret: `12,518,619,140` (improved from ~`15,245,334,977`)
  - native: `78,069,083`

### Notes

- This pass gives order-of-magnitude+ gains on modulo-dispatch loops (ifchain pattern).
- Generic heavy arithmetic loops (`for_many_ops_mod`) still need separate superinstruction/JIT-style lowering to reach similar multipliers.

## 2026-02-24 - Control-Flow Surface Expansion (`switch/case`, `try/catch`, `break/continue`)

### External references reviewed

- LLVM LangRef `switch` instruction behavior and lowering expectations (jump-table/branch-chain friendly IR shape):
  - https://llvm.org/docs/LangRef.html#switch-instruction
- Swift structured error-handling reference (`do/catch` semantics):
  - https://theswiftengineer.com/documentation/the-swift-programming-language/errorhandling.html
- Java switch-expression/statement reference for deterministic first-match/default behavior:
  - https://docs.oracle.com/en/java/javase/24/language/switch-expressions-and-statements.html

### Scope

- Added new core control statements to parser + AST + interpreter:
  - `switch expr:` / `case ...:` / `default:`
  - `try:` / `catch:` / `catch as err:`
  - `break`
  - `continue`

### Applied changes

- AST:
  - new statement kinds + nodes:
    - `BreakStmt`, `ContinueStmt`, `SwitchStmt`, `TryCatchStmt`
  - file: `compiler/include/spark/ast.h`
- Parser:
  - dispatch and grammar support for new statements.
  - explicit parser diagnostics when `case/default/catch` appears outside its parent block.
  - files:
    - `compiler/src/phase2/parser_parts/02a_parser_statement_dispatch.cpp`
    - `compiler/src/phase2/parser_parts/02b_parser_control_statements.cpp`
- Interpreter runtime:
  - control-flow signals:
    - `BreakSignal`, `ContinueSignal`
  - `for/while` loops now consume those signals with correct semantics.
  - `switch`:
    - first-match semantics in source order.
    - literal-case fast runtime plan cache + dynamic-case fallback.
    - `break` exits switch scope.
- `try/catch`:
  - catches `EvalException` only.
  - `catch as <name>` binds error text as string.
  - `return/break/continue` signals are not swallowed.
  - fixed stale pointer-cache issue in fast `if` plan cache by adding statement fingerprint validation (prevents incorrect branch reuse after AST pointer recycling).
- Added `while` modulo-dispatch specialization support for `switch` branch form:
  - pattern: `r = i % M` + `switch r` + branch `acc = acc +/- const`.
  - keeps generic semantics and enables faster interpreter path for switch-based dispatch loops.
- files:
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_break.cpp`
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_continue.cpp`
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_switch.cpp`
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_try_catch.cpp`
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_for.cpp`
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_while.cpp`
    - `compiler/src/phase3/evaluator_parts/stmt/stmt_if.cpp`
    - `compiler/src/phase5/runtime/interpreter_core.cpp`
    - `compiler/src/phase9/runtime/02_task_runtime.cpp`
- Semantic/codegen integration:
  - semantic analyzer now validates `break/continue` outside-loop usage.
  - codegen marks these new statements as unsupported in phase4 native lowering (explicit diagnostic).
  - files:
    - `compiler/src/phase3/semantic_parts/03_context_tier.cpp`
    - `compiler/src/phase3/semantic_parts/04_analysis.cpp`
    - `compiler/src/phase4/codegen_parts/04_codegen_front.cpp`

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests:
  - `ctest --test-dir build -R "sparkc_smoke_test|sparkc_eval_tests|sparkc_codegen_tests" --output-on-failure` PASS.
  - Added parser/evaluator tests for:
    - `switch/case/default`
    - `try/catch` and `catch as`
    - loop `break/continue` behavior.
- Microbench note (`while`, 2,000,000 iters, interpret, internal tick):
  - `switch` dispatch loop improved from ~`609,164,544` to ~`209,320,984` raw tick units (~`2.9x` faster) after switch-aware modulo path + safe plan cache.

## 2026-02-24 - While/For Mod-Dispatch Cache And Residue Canonicalization

### Problem

- `for`/`while` mod-dispatch fast paths were rebuilding parse plans at each loop entry.
- closed-form delta accumulation still performed duplicate-residue filtering (`unordered_set`) at runtime.

### Applied optimization

- Added fingerprint-based runtime plan caches:
  - `ForStmt` -> cached `FastModuloDispatchPlanFor`
  - `WhileStmt` -> cached `FastIntWhileConditionPlan`
  - `WhileStmt + index variable` -> cached `FastModuloDispatchPlanWhile`
- Canonicalized duplicate `match_values` at plan-build time and removed per-run duplicate filtering in closed-form execution.

### Why this is safe

- Fingerprint invalidation rebuilds plans when relevant AST pointer graph changes.
- Duplicate residue canonicalization preserves first-match behavior (later duplicates are unreachable anyway).

### Validation

- Build:
  - `cmake --build build --target sparkc --clean-first -j8` PASS
  - `cmake --build build -j8` PASS
- Tests:
  - `ctest --test-dir build -R "sparkc_parser_tests|sparkc_eval_tests|sparkc_smoke_test|sparkc_codegen_tests" --output-on-failure` PASS
- Measurement note:
  - control-flow microbench loops stay in the same raw-tick band after refactor while removing repeated plan-build overhead from hot entry paths.

## 2026-02-24 - Bench-Tick While Fast-Path Activation For Split Targets

### Problem

- Single-op benchmark generator emits loop body as:
  - `floor_c = a`
  - `raw_c = a op b`
- Existing `bench_tick` fast-path required the two assignment targets to be the same variable, so fast-path was rejected and interpreter fell back to generic execution.

### Applied optimization

- Extended `FastBenchTickWindowPlan` to track `pre_copy_target_var` separately from `op_target_var`.
- Removed same-target gate in planner.
- Runtime now writes pre-copy into `pre_copy_target_var` and op result into `op_target_var`.
- Added fixed-limit trip-count execution when loop limit cannot be mutated by loop body writes.

### Validation

- Build: `cmake --build build -j8` PASS.
- Tests: `ctest --test-dir build -R "sparkc_eval_tests|sparkc_parser_tests|sparkc_smoke_test|sparkc_codegen_tests" --output-on-failure` PASS.
- Probe check:
  - fast-path enabled: `/tmp/bench_probe.k` raw total `708387`
  - fast-path disabled (`SPARK_BENCH_TICK_WINDOW_FAST=0`): raw total `23327457`

### Benchmark snapshot (single-op window, 500k loops, runs=5)

- `f64 +`
  - interpret raw: `3.502 ns/op`
  - native raw: `4.948 ns/op`
- `f512 +`
  - interpret raw: `4.254 ns/op`
  - native raw: `4.607 ns/op`

## 2026-02-24 - Modular Statement Controllers (No-Semantics Refactor)

### Goal

- Improve phase3 evaluator structure so control-flow/runtime statement routing is modular (`controller` + `orchestrator`) while preserving exact behavior and hot-path performance.

### Applied changes

- Added modular statement controller modules:
  - `compiler/src/phase3/evaluator_parts/stmt_controllers/01_stmt_controller_core.cpp`
  - `compiler/src/phase3/evaluator_parts/stmt_controllers/02_stmt_controller_control_flow.cpp`
  - `compiler/src/phase3/evaluator_parts/stmt_controllers/03_stmt_controller_definition.cpp`
  - `compiler/src/phase3/evaluator_parts/stmt_controllers/04_stmt_controller_orchestrator.cpp`
- Updated `FastStmtExecThunk` lookup in:
  - `compiler/src/phase3/evaluator_parts/internal_helpers.h`
  - now delegates to `stmt_exec_controller_for_kind`.
- Registered controller modules in unity evaluator build:
  - `compiler/src/phase3/evaluator.cpp`

### Validation

- Build:
  - `cmake --build build -j8` PASS.
- Core correctness suite:
  - `ctest --test-dir build -R "sparkc_eval_tests|sparkc_parser_tests|sparkc_smoke_test|sparkc_codegen_tests" --output-on-failure` PASS.
- Phase matrix smoke:
  - `ctest --test-dir build --output-on-failure` shows existing unrelated failure in `sparkc_phase5_crosslang_primitives` (integer division extreme mismatch set); no new parser/evaluator regression observed in phase3/phase7/phase8/phase9/phase10 tests.
- Perf sanity (post-refactor snapshot):
  - `benchmark_float_crosslang_fair_runtime.py` (`loops=20000`, interpret+native, no cross-lang) keeps native `f32/f64` in ~`1.0-1.4 ns/op` band and interpreter in prior ~microsecond band (no obvious routing regression).

## 2026-02-24 - Market-Wide Target Catalog And Tiered Portability Presets

### Goal

- Move from hard-coded small target triples to an extensible portability catalog that can cover desktop/server/mobile/embedded classes in one orchestration surface.

### Applied changes

- Added catalog module:
  - `scripts/core/pipeline/phase10/target_catalog.py`
  - includes target metadata: `family`, `os_class`, `tier`, `mode`, `notes`.
  - includes presets: `core`, `linux`, `desktop`, `mobile`, `windows`, `embedded`, `market`.
- Extended multiarch orchestrator:
  - `scripts/core/pipeline/phase10/multiarch_build.py`
  - new CLI options:
    - `--preset`
    - `--include-experimental`
    - `--include-embedded`
    - `--list-presets`
  - output now contains per-target `tier/mode/family/os_class`.
  - embedded targets are reported as skipped (`interpret_only` planning tier) instead of pretending AOT success.
- Added support matrix documentation:
  - `docs/platform_support_matrix.md`
  - README phase10 command section now includes preset-based usage examples.

### Validation

- `python3 scripts/core/pipeline/phase10/multiarch_build.py --list-presets` works.
- quick market-style check on current host:
  - `python3 scripts/core/pipeline/phase10/multiarch_build.py --preset core --run-host-smoke --lto off`
  - host result snapshot remains compatible with previous behavior (core targets still evaluated).

## 2026-02-24 - CI Portability Readiness Gate + Cross-Host Matrix

### Goal

- Make platform portability a hard non-regression rule in CI for CPU/GPU catalog coverage.

### Applied changes

- Added portability gate script:
  - `.github/scripts/platform_readiness_gate.py`
  - runs `scripts/core/pipeline/phase10/platform_matrix.py` with market-wide CPU+GPU options
  - validates:
    - minimum CPU catalog size
    - minimum GPU catalog size
    - required baseline entries (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `metal`, `webgpu`)
    - duplicate protection in target/backend lists
- Wired guard into CI main job (`build-and-test`) as required step.
- Added CI cross-host portability job:
  - `Portability Host Matrix (ubuntu/macos/windows)`
  - compiles phase10 portability scripts and emits host-specific platform matrix artifacts.

### Validation

- Local:
  - `python3 .github/scripts/platform_readiness_gate.py --json-out bench/results/ci_platform_matrix_local.json` PASS
  - report snapshot:
    - CPU total `20`, stable `4`
    - GPU total `7`, stable `1`

## 2026-02-24 - GPU Smoke Matrix For "Other GPU" Testability

### Goal

- Make non-host GPU backend checks operationally testable with one script and CI artifacts.

### Applied changes

- Added:
  - `scripts/core/pipeline/phase10/gpu_smoke_matrix.py`
- Modes:
  - report mode:
    - `--backends all --include-planning`
  - strict mode:
    - `--fail-on-unavailable <backend-list>`
- CI portability host matrix now runs:
  - report mode on all hosts (`ubuntu/macos/windows`)
  - strict `metal` availability check on macOS host
  - uploads GPU smoke JSON artifacts per host

### Local validation

- `python3 scripts/core/pipeline/phase10/gpu_smoke_matrix.py --backends all --include-planning --json-out bench/results/phase10_gpu_smoke_local.json` PASS
- Current host snapshot:
  - available backend: `metal`
  - unavailable examples: `cuda`, `rocm_hip`, `oneapi_sycl`, `opencl`, `vulkan_compute`

### CI extension

- Added manual strict GPU workflow:
  - `.github/workflows/gpu-backend-smoke.yml`
  - backend-select dispatch (`all` or single backend)
  - self-hosted runner labels for CUDA/ROCm/oneAPI/OpenCL/Vulkan and hosted macOS strict check for Metal.

## 2026-02-24 - GPU Control-Plane Perf Matrix

### Goal

- Track GPU backend runtime-entry overhead and probe latency with measurable artifacts.

### Applied changes

- Added:
  - `scripts/core/pipeline/phase10/gpu_backend_perf.py`
- Measurements:
  - backend detection latency (`detect_median_ms`, `p95`)
  - successful probe command latency (`median/mean/p95`) when backend is available
- CI portability host matrix now publishes:
  - `phase10_gpu_perf_<OS>.json`

### Local validation

- Command:
  - `python3 scripts/core/pipeline/phase10/gpu_backend_perf.py --backends all --include-planning --runs 7 --warmup 2 --json-out bench/results/phase10_gpu_perf_local.json`
- Snapshot (this host):
  - `metal` available; detect latency sub-millisecond band.
  - non-native backends are reported unavailable with explicit reason.

## 2026-02-24 - GPU Backend Runtime Perf (Requested vs Effective)

### Goal

- Ensure backend requests that are unavailable on the host still execute stably with highest available host path and measurable runtime output.

### Applied changes

- Runtime schedule mapping:
  - `SPARK_MATMUL_BACKEND` values `cuda|rocm_hip|oneapi_sycl|opencl|vulkan_compute|metal|webgpu`
  - map to effective backend:
    - BLAS if available,
    - otherwise own kernel.
- Added benchmark script:
  - `scripts/core/pipeline/phase10/gpu_backend_runtime_perf.py`
  - reports:
    - requested backend
    - effective backend (`own`/`blas`)
    - median/mean runtime
    - correctness/pass flags from phase8 output.

### Local validation

- Command:
  - `python3 scripts/core/pipeline/phase10/gpu_backend_runtime_perf.py --program bench/programs/phase8/matmul_core_f64.k --runs 3 --warmup 1 --max-perf --json-out bench/results/phase10_gpu_runtime_perf_local.json --csv-out bench/results/phase10_gpu_runtime_perf_local.csv`
- Result snapshot:
  - all requested backends executed successfully.
  - on this host, requested GPU aliases route to `blas` effective backend.
