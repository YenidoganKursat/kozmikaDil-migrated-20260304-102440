# Container Model

Date: 2026-02-17

## Semantic Types
- `List` and `Matrix` are semantic container abstractions.
- physical representation can change without changing source-level behavior.

## List Representations
- `Packed[T]`:
  - homogeneous, unboxed contiguous storage
- `PromotedPacked[f64]`:
  - hetero-numeric inputs normalized to `f64`
- `ChunkedUnion`:
  - type-run segmentation for hetero iteration-heavy workloads
- `Boxed[Any]`:
  - fully dynamic fallback representation
- `GatherScatter`:
  - typed compute path that preserves external element order

## Matrix Representations
- `Matrix[T]` contiguous (row-major canonical storage)
- `MatrixView[T]` with offset/stride metadata
- packed matmul buffers for kernel execution (`Phase 8` pack cache)

## State Machine (Analyze -> Plan -> Materialize -> Cache)
1. analyze container shape/type/layout
2. choose plan for operation
3. materialize only when required
4. cache plan/materialized state
5. invalidate on mutation

## Selection Signals
- homogeneity
- numeric promotability
- iteration vs random-access profile
- operation type (elementwise/reduce/matmul/pipeline)

## Explainability
- `k run --explain-layout file.k`
- `k analyze --dump-layout file.k`
- runtime stats:
  - list/matrix cache counters
  - plan id
  - matmul backend and pack cache counters
