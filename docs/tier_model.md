# Tier Model

Date: 2026-02-17

## Tier Definitions
- `T8` (surface/dynamic):
  - maximal language flexibility
  - dynamic shapes, heterogeneous containers, fallback-friendly behavior
- `T6` (adaptive runtime):
  - analyze-plan-materialize-cache for container layouts
  - retains dynamic semantics with guarded optimization
- `T4` (performance core):
  - statically stabilizable regions
  - known type/shape/effect contracts for native lowering
- `T2` (kernel tier):
  - compiler/runtime internal tight kernels
  - restricted semantics, no user-facing dynamism

## Eligibility Rules (current)
- `T4` requires:
  - stable element types inside hot loops
  - no unknown side-effect escapes that break alias/effect assumptions
  - shape-stable object/container access in the analyzed region
- `T5` is intermediate:
  - normalizable to `T4` with layout/type promotion
- `T8` fallback:
  - unsupported/per-element dynamic behavior remains executable in interpreter/adaptive runtime.

## Diagnostics
- `k analyze file.k --dump-tier`
  - reports per function and loop tier.
- non-`T4` regions include explicit reasons (`Any` list element in hot loop, open shape, unknown effects, etc.).

## Runtime Bridge
- `k run`:
  - tries native path for eligible regions
  - falls back to interpreter for blocked regions
- `k build`:
  - emits AOT binary for target-triple path; tier blockers are surfaced before emission.

## Phase 10 Additions
- cross-target build controls: `--target`, `--sysroot`
- final profiling/perf controls: `--lto`, `--pgo`, `--pgo-profile`
- layout and dispatch visibility:
  - `--dump-layout` / `--explain-layout`
  - `--print-cpu-features`
