# Benchmark Plan (Phase 1)

## Active C Baselines

- `B1`: `bench_scalar.c`
- `B2`: `bench_list.c` (packed list-equivalent vector iteration)
- `B4`: `bench_matrix_elemwise.c` (elementwise matrix kernel)
- `B5`: `bench_matmul.c` (naive matrix multiplication correctness + timing)

## Planned Placeholders (next phases)

- `B3`: heterogeneous list normalization and steady-state pipeline behavior.
- `B6`: object create/access/update/copy/delete loops.
- `B7`: async/event overhead.
- `B8`: parallel map/reduce scaling.

## Required outputs

- `phase1_raw.json`: benchmark outputs with PASS/FAIL + checksums.
- `phase1_raw.csv`: machine-readable flat table for quick comparisons.
- `hyperfine.json`: optional distribution sample for wall-time confidence intervals.
- `perf_stats.txt`: optional micro-architectural counters.
