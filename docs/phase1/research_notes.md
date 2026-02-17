# Research Notes (Phase 1)

## Core anchors for Phase 1 baseline and later compiler/core design

- MLIR (multi-level IR model)
  - Role now: establish toolchain direction for LLVM lowering and future dialect layering.
  - Phase mapping: `Phase 1` decision, `Phase 2+` integration candidate.

- Halide schedule/algebra separation
  - Role now: define a roadmap for algorithm-vs-schedule split in the perf tier.
  - Phase mapping: `Phase 3` for kernel schedule design.

- Weld IR/data-parallel fusion
  - Role now: baseline concept for cross-operator fusion across list/matrix pipelines.
  - Phase mapping: `Phase 2` (list pipeline fusion prototypes), `Phase 3` (production IR fusion passes).

- Stream Fusion
  - Role now: eliminate intermediate list representations in map/filter/fold style operators.
  - Phase mapping: `Phase 2` (container op semantics), `Phase 3` (typed lowering).

- Futhark-style functional array model
  - Role now: guide data-parallel array abstraction semantics and transformation safety.
  - Phase mapping: `Phase 3` (typed array kernel path).

- Triton / TVM
  - Role now: long-term inspiration for matmul and tensor/tiled kernels with autotuning.
  - Phase mapping: `Phase 4` (specialized kernel backends).

- Hidden classes + inline caches
  - Role now: object/record access optimization strategy in perf tier.
  - Phase mapping: `Phase 4` (object model pass 1).

- Region-based memory models / generational mix
  - Role now: define memory ownership strategy for temporary/pipeline allocations.
  - Phase mapping: `Phase 2` design, `Phase 3` runtime implementation.

- Cilk work-stealing + structured concurrency semantics
  - Role now: model for low-overhead parallel task scheduling.
  - Phase mapping: `Phase 4` runtime scheduling.

- AutoFDO + BOLT + Propeller
  - Role now: build-time heavy optimization path for final performance tiers.
  - Phase mapping: `Phase 5` build/release pipeline hardening.

## Why these anchors (justification)

- Phase 1 currently needs stable measurement, correctness gates, and clean baselines.
- The listed anchors are selected for low-fragility, proven performance ideas that can be introduced incrementally.
- Early phases favor deterministic C baselines and reproducible measurements before introducing language semantics complexity.
