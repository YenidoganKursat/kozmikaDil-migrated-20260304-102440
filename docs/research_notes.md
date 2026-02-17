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
