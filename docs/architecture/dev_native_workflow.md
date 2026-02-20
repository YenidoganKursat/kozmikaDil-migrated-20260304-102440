# Development + Native Performance Workflow

This workflow keeps development ergonomics while preserving native performance validation.

## Modes

- Correctness-first loop:
  - `k run --interpret file.k`
  - fast iteration, parser/semantic/runtime debugging.
- Native runtime loop:
  - `k build file.k -o out.bin`
  - run `out.bin` repeatedly for runtime-only measurements.
- Differential safety loop:
  - compare `interpret` and `native` outputs on the same inputs.

## Benchmark Policy

1. Report runtime separately from build time.
2. State backend explicitly for every benchmark output:
   - `interpret`, `native`, or `builtin`.
3. Treat `builtin` as micro-kernel stress mode only.
4. Use `native` for real production performance claims.

## Validation Policy

- Unit tests must cover phase-local semantics and edge cases.
- Randomized checks should be reproducible (fixed seed or logged seed).
- Extreme cases (zero, subnormal-like values, sign flips, divide-by-near-zero, exponent edge cases) must be included.

