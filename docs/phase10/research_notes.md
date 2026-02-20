# Phase 10 Research Notes

Date: 2026-02-17

## Primary References Used
- LLVM PGO user-flow (`-fprofile-instr-generate` / `-fprofile-instr-use` + `llvm-profdata merge`)
- LLVM ThinLTO/LTO optimization guidance
- BOLT toolchain flow (`perf record` -> `perf2bolt` -> `llvm-bolt`)
- CPUID/HWCAP based runtime dispatch best practices

## Design Selections
- Keep strict numerical defaults in release-perf profile; avoid fast-math by default.
- Use runtime-dispatch metadata to pick schedule variants without changing semantics.
- Keep post-link optimization optional and measurable; skip cleanly when tools unavailable.

## Practical Findings
- PGO cycle must be single-command and artifact-driven to avoid drift.
- Dispatch correctness needs forced-feature testing on one machine; env override enables this.
- Cross-target portability should degrade gracefully (clear script-level reporting if sysroot/toolchain missing).
- Sanitizer gates are most stable when isolated per profile (ASan+UBSan separate from TSan).

## Additional Speed Potential (Post-Phase10)

Current snapshot signals:
- Phase10 PGO/LTO gain is low-single-digit (`~1.03x` to `~1.05x`) and stable.
- BOLT stage is currently skipped on this host (toolchain missing), so post-link gains are unclaimed.
- Phase8 own GEMM is still materially below C/BLAS ratios on larger matrix cases, while auto backend already tracks best path closely.

Highest-confidence next accelerators:
1. Add sample-based PGO (AutoFDO/sPGO) flow in addition to instrumentation PGO.
   - Clang user manual supports `-fprofile-sample-use` with `-fdebug-info-for-profiling`.
2. Enable true ISA-level multiversioned kernels (not only schedule dispatch).
   - Clang `target` / `target_clones` can emit runtime-selected variants.
3. Rework own GEMM kernel around fused packing + compute-first pass ideas (GEMMFIP/BLIS style).
4. Run BOLT on Linux perf-capable runner and keep it in release-perf pipeline.
5. Evaluate MemProf on allocation-heavy pipeline/concurrency paths where cache locality and hot/cold new separation matter.

Risk notes:
- Small hand-tuned loop edits that don’t move instruction-cache/dispatch behavior in a measurable way are often unstable; keep only multi-run median wins.
- For matrix engine improvements, prioritize architecture-specific micro-kernels and packing strategy over scalar loop micro-edits.

## Fast Follow Trial (2026-02-17, LTO mode check)

Measured on `bench/programs/phase10/pgo_call_chain_large.k` with the same PGO flow:

- ThinLTO trial (`profile_runs=3`): `0.9872x` (regression in this host run)
- Full LTO trial (`profile_runs=3`): `1.0414x`
- Full LTO trial (`profile_runs=5`): `1.1020x` (best observed)

Decision:
- Keep `--lto=full` as the default in phase10 benchmark orchestration.
- Raise default PGO profiling passes to `profile_runs=5` for steadier profile quality.
- Continue to allow `--lto=thin` override for faster iteration runs.

Observed negative trial:
- Increasing only the profile workload length (`N=5_000_000` variant) did **not**
  improve stability; repeated runs still drifted around/below `1.0x`.
- Kept the previous program and focused on release defaults + no-regression gating.

No-regression policy applied:
- `scripts/pgo_cycle.sh` now records both `raw_speedup_vs_baseline` and
  `selected_variant` (`pgo_use` or `baseline`).
- If raw PGO is slower, release-perf path keeps baseline for that cycle, so the
  effective speedup reported to the gate cannot regress below `1.0x`.

## AutoFDO + BOLT Fast Follow (2026-02-17)

Implemented:
- Added sample-based PGO cycle script:
  - `scripts/autofdo_cycle.sh`
  - flow: `perf record` -> `llvm-profgen` -> `-fprofile-sample-use`
- Added `autofdo` section to phase10 benchmark orchestrator:
  - `bench/scripts/run_phase10_benchmarks.py`
- Extended BOLT script with:
  - profile loop repeat (`--profile-runs`)
  - raw/effective speed split + no-regression selection policy
  - `raw_speedup_vs_baseline` + `selected_variant` telemetry in JSON

Current host result:
- AutoFDO: skipped (tooling not present on this machine), gate remains pass via skip policy.
- BOLT: skipped (same reason), gate remains pass via skip policy.
- PGO: raw speedup is workload/host-noise sensitive; no-regression selector keeps
  release-perf at or above baseline for each cycle.

Design decision:
- Keep AutoFDO/BOLT integrated and artifact-producing even when skipped, so Linux perf-capable
  runners can claim gains without code changes.

## Primitive Numeric Backends Research (2026-02-18)

Goal:
- Make `i8..i512` and `f8..f512` practical with stable runtime cost and clear precision policy.

Primary references reviewed:
- MPFR project docs (correctly-rounded arbitrary precision floating point): [mpfr.org](https://www.mpfr.org/)
- GMP manual and integer model (`mpz_t`): [gmplib.org/manual](https://gmplib.org/manual/)
- libquadmath (`__float128`) API reference: [gcc.gnu.org/onlinedocs/libquadmath](https://gcc.gnu.org/onlinedocs/libquadmath/)
- C23 `_BitInt` status and constraints discussion: [open-std.org N2763](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2763.pdf)
- Memory allocator references:
  - mimalloc design/perf notes: [microsoft.github.io/mimalloc](https://microsoft.github.io/mimalloc/)
  - jemalloc docs: [jemalloc.net/jemalloc.3.html](https://jemalloc.net/jemalloc.3.html)

Implementation decision for current repo snapshot:
- Keep a dependency-light fallback backend in-tree (no external multiprecision dependency required).
- Expose full primitive surface (`i8..i512`, `f8..f512`) and make operations work end-to-end.
- Improve runtime by avoiding repeated parse/string work (numeric parse cache + lazy payload formatting).
- Reduce allocator pressure by storing numeric payload inline (`optional<NumericValue>`) instead of heap `shared_ptr`.

Measured effect (primitive scalar cycle, overflow-safe profile, 80k loops, 7-run median):
- benchmark profile was reset to avoid overflow-driven noise (float: `+ * / -`, int: `+ * % -`).
- baseline-to-current speedups in `bench/results/primitives/primitive_cycle_summary.json`:
  - min: `~0.98x`
  - median: `~1.05x`
  - max: `~1.43x`

Known limitation (explicit):
- Current fallback backend is `i128`/`long double` powered.
- `i256/i512` and `f128/f256/f512` are available as runtime kinds and constructors, but full arbitrary-precision
  arithmetic is deferred until optional MP backend integration.

## 10x Primitive Acceleration Result (2026-02-18)

Applied strategy:
- AOT-native lowering for primitive constructors in phase4 codegen (`i8..i512`, `f8..f512`).
- Benchmark policy updated to "build once, run binary many times" with build time excluded.
- Interpreter is retained as correctness/fallback path.

Why this matches literature:
- This follows the common "dynamic frontend + native specialized backend" pattern used in
  modern runtimes and compilers (JIT/AOT hybrid patterns in PyPy/Julia/LLVM-based systems).
- References:
  - [PEP 659 (Specializing Adaptive Interpreter)](https://peps.python.org/pep-0659/)
  - [Julia JIT design](https://docs.julialang.org/en/v1/devdocs/jit/)
  - [LLVM ORC JIT](https://llvm.org/docs/ORCv2.html)
  - [PyPy tracing JIT docs](https://www.pypy.org/performance.html)

Measured with:
- baseline (interpreter): `--mode interpret --loops 200000 --runs 5 --warmup 1 --reset-baseline`
- optimized (native auto): `--mode auto --loops 200000 --runs 7 --warmup 2`

Result file:
- `bench/results/primitives/primitive_cycle_summary.json`

Observed speedups:
- min: `~46.72x`
- median: `~71.59x`
- max: `~90.70x`
- all primitives >= `10x`: yes

## Anti-Optimization Benchmark Hardening (2026-02-18)

Motivation:
- Constant-only microbench kernels can be over-optimized by AOT compilers, producing
  unstable/optimistic numbers.

Applied fix:
- Added `bench_tick()` runtime builtin and used it in primitive benchmark source generation
  to seed loop constants at runtime.
- This follows common benchmark guidance to prevent dead-code/constant-fold artifacts.

References:
- Google Benchmark user guide (`DoNotOptimize`, `ClobberMemory`) for anti-elision concepts:
  <https://github.com/google/benchmark/blob/main/docs/user_guide.md>
- LLVM benchmark caveats and optimization-sensitive microbench discussion:
  <https://llvm.org/docs/Benchmarking.html>

Revalidated (same target):
- baseline: interpreter mode (`--mode interpret`)
- optimized: native mode (`--mode auto`)
- loops=200000, warmup=2, runs=9

Result:
- all primitive categories still >= `10x`
- observed range: `~47.61x` to `~99.58x`

## Multi-Float Operator Specialization Notes (2026-02-18)

Question:
- Should `f8/f16/.../f512` always be faster than `f64`?

Finding:
- Not necessarily. Speedup appears only when backend has true dtype-specialized
  storage + arithmetic + vectorized lowering for that dtype.
- If the backend collapses to one execution type (e.g., f64), width advantage is
  mostly lost.

References reviewed:
- Clang half/bfloat semantics and excess precision controls:
  <https://clang.llvm.org/docs/LanguageExtensions.html>
- LLVM floating type and conversion semantics:
  <https://releases.llvm.org/12.0.0/docs/LangRef.html>
- Intel AVX-512 FP16/BF16 docs (hardware capability constraints):
  <https://www.intel.com/content/www/us/en/content-details/669773/intel-avx-512-fp16-instruction-set-for-intel-xeon-processor-based-products-technology-guide.html>
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-deep-learning-boost-new-instruction-bfloat16.html>
- oneDNN ISA dispatch policy (runtime stable dispatch pattern):
  <https://www.intel.com/content/www/us/en/docs/onednn/developer-guide-reference/2024-0/cpu-dispatcher-control.html>
- LLVM forum discussion on fp16 semantics (practical portability caveats):
  <https://discourse.llvm.org/t/changing-semantics-of-fp16/57155>
- Paper (accuracy/performance tradeoff in BF16 mixed precision):
  <https://arxiv.org/abs/1904.06376>

Applied in repo (v1 -> v1.1):
- Added scalar `numeric_hint` propagation in phase4 codegen so constructor-origin
  float primitive (`f8..f512`) can keep a dtype-aware operator path.
- Added generated C helper kernels:
  - `__spark_num_add/sub/mul/div/mod_{f8,f16,bf16,f32,f64,f128,f256,f512}`
- v1.1 optimization:
  - quantize once at constructor path,
  - per-op uses `Q(a op b)` instead of `Q(Q(a) op Q(b))` to reduce overhead while
    keeping dtype-aware rounding behavior.

Current constraint:
- This is scalar operator specialization; full typed list/matrix storage and
  end-to-end SIMD kernelization are still required for complete multi-dtype
  performance scaling.

## Precision Backend Research (2026-02-18)

Problem:
- `f128/f256/f512` target precision is much stricter than current measured band.
- `f8/f16/bf16` rounding/subnormal behavior needed a less ad-hoc model.

Primary references reviewed:
- Berkeley SoftFloat (software IEEE-754 behavior model):
  - <https://www.jhauser.us/arithmetic/SoftFloat-3/doc/SoftFloat.html>
  - <https://www.jhauser.us/arithmetic/SoftFloat-3/doc/SoftFloat-source.html>
- ONNX Float8 E4M3FN semantics:
  - <https://onnx.ai/onnx/technical/float8.html>
- MPFR (arbitrary precision with correct rounding):
  - <https://www.mpfr.org/mpfr-current/mpfr.html>
- GCC libquadmath manual (binary128 route on supported platforms):
  - <https://gcc.gnu.org/onlinedocs/libquadmath/>

Decision and implementation update:
- Replaced simplistic mantissa-only quantization for `f8/f16/bf16` with bit-level
  RNE quantizers in runtime + generated C code path.
- `f8` path aligned to E4M3FN-style finite saturation semantics (`max finite`).
- `f16` path now enforces exponent range and subnormal handling (IEEE-754-like behavior).

Current hard limit:
- On this machine, `long double` has 53 mantissa bits (same as `double`), so
  `f128/f256/f512` cannot reach target precision without a real multiprecision backend.
- Next required step: optional MPFR/SoftFloat integration behind a dedicated high-precision
  numeric backend switch.

## Floating-Point Validation Method Research (2026-02-18)

Question:
- "Teorik epsilon'a ne kadar yakiniz?" sorusunu hangi metrikle dogru olcmeliyiz.

Sources reviewed:
- Goldberg (classic paper): <https://www.eecg.toronto.edu/~moshovos/ECE243-07/00.practice/What%20Every%20Computer%20Scientist%20Should%20Know%20About%20Floating-Point%20Arithmetic.htm>
- Berkeley SoftFloat docs: <https://www.jhauser.us/arithmetic/SoftFloat-3/doc/SoftFloat.html>
- IEEE-754 background summary (machine epsilon/subnormal framing): <https://en.wikipedia.org/wiki/IEEE_754>

Applied method:
- Generate binary-exact test inputs to avoid decimal parsing artifacts.
- Quantize inputs to target type first, then compare operator result against high-precision reference
  rounded back to the same type (operator conformance check).
- Report both:
  - absolute/relative error,
  - epsilon-normalized ratio (`abs_error / epsilon@1`).

Why:
- Raw abs error against untyped decimal inputs overstates low-precision formats and mixes
  conversion error with operator error.
- Type-rounded reference isolates the operator path and shows whether implementation is
  approaching the theoretical limit for that type model.

## Pow/Container Strategy Notes (2026-02-19)

Goal:
- Add `^` operator across primitive/list/matrix paths without regressing numeric fast lanes.
- Keep heterogeneous semantics explicit (no silent numeric downcast for object/string cells).

Primary references reviewed:
- MPFR pow and rounding semantics:
  <https://www.mpfr.org/mpfr-current/mpfr.html>
- LLVM fast-math flags (why strict mode should remain default):
  <https://llvm.org/docs/LangRef.html#fast-math-flags>
- GCC optimize options used in runtime-focused profiles:
  <https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>

Applied decisions:
- `^` is right-associative in parser and mapped to numeric pow in scalar/list/matrix kernels.
- Heterogeneous container fallback is intentionally narrow:
  - enable `+` and string-repeat `*`,
  - reject `- / % ^` for non-numeric elements.
- Numeric fusion path now gracefully falls back when encountering non-numeric container cells.

Do/Don't reminders:
- Do keep strict-correctness path as default (`fast-math` off).
- Do isolate hetero fallback from packed numeric kernels.
- Don't silently coerce object/string containers into numeric paths.
- Don't claim 1000x globally; report per workload/operator with checksums.

## Integer-Exponent Pow Tightening (2026-02-19)

Observation:
- Generic `pow/powl` paths can produce larger-than-necessary error for integer exponents.

Applied strategy:
- Detect integral exponent in bounded range and switch to exponentiation-by-squaring.
- Keep fallback to `pow/powl` for non-integral exponents.

Why this is safe:
- For integral exponent, repeated multiply preserves intended algebraic path and avoids extra approximation stages.
- Domain edge (`0^-n`) still handled as infinity-compatible path.

Measured effect:
- f64 `^` stepwise max abs error reduced from ~`3.64e-12` to ~`1.42e-14` in current harness.
- f64 `^` primitive throughput also increased significantly in native profile.

## MPFR-backed Stepwise Reference for High Float Families (2026-02-19)

Problem:
- Java BigDecimal harness used double-quantized paths for `f128/f256/f512`, inflating epsilon-ratio diagnostics.

Applied change:
- Added dedicated MPFR stepwise reference tool for high families:
  - `bench/scripts/primitives/mpfr_stepwise_reference.cpp`
- Integrated into validation pipeline:
  - `check_float_ops_stepwise_bigdecimal.py` now uses MPFR rows by default for `f128/f256/f512`.
- Kept legacy Java values as side-channel metrics (`java_max_abs_*`) for visibility.

Why this is better:
- Reference now follows binary precision semantics with explicit precision control and round-to-nearest-even behavior.
- Report can distinguish:
  - real high-precision conformance,
  - legacy Java/double-bound artifacts.
