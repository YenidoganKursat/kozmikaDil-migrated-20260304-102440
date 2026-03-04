# Memory Model

Date: 2026-02-17

## Objectives
- keep hot-path allocations bounded
- keep correctness-first semantics in strict mode
- preserve predictable behavior under async/parallel workloads

## Runtime Allocation Strategy (current)
- Core values (`int`, `double`, `bool`, small list metadata) are direct value objects.
- Lists and matrices maintain heap-backed buffers with cache metadata.
- Phase 6+ layout caches:
  - `analyze_count`
  - `materialize_count`
  - `cache_hit_count`
  - `invalidation_count`

## Container Materialization Rules
- normalize/materialize only when plan requires it.
- mutation operations invalidate cached plans.
- views/slices preserve semantics and can trigger materialization if kernel path requires contiguous layout.

## Matrix Pack Cache
- Phase 8 pack cache stores packed A/B buffers for repeated matmul.
- cache hit/miss counters exposed via `matmul_stats()`.
- pack cache is versioned by source matrix shape/layout state.

## Async/Parallel Memory Safety (Phase 9)
- concurrent closures execute with frozen capture environments on runtime fast path.
- bounded channels enforce backpressure and prevent unbounded queue growth.
- scheduler uses task ownership boundaries; shared mutable state is restricted by diagnostics.

## Safety Profiles
- strict numerical path is global and mandatory.
- relaxed/approximate floating-point optimization modes are not allowed.
- sanitizer gates (ASan/UBSan/TSan builds) are provided via Phase 10 scripts.
- differential checks validate optimized output vs interpreter output on representative suites.
