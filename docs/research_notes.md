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
