# Reproducibility Policy (Phase 1)

1. Every benchmark in `bench/benchmarks` is deterministic.
2. Runner executes binaries on the same machine and records `phase1_raw.json` and `phase1_raw.csv`.
3. Recommended execution rule:
   - run each benchmark at least 5 times.
   - keep one non-timed correctness-only run for `pass/checksum/expected` capture.
   - run `RUNS` (currently 7) timed runs in `bench/scripts/run_phase1_baselines.sh`.
   - sort timings, trim one minimum and one maximum, then compute median/drift.
4. Acceptance rule:
   - `max(|t_i - median| / median) * 100 <= 3.0` for trimmed timed runs.
   - `reproducible` field in JSON/CSV must be `1` for all baselines.
5. If drift > 3.0% on unchanged inputs/commit, investigate CPU scaling governor, thermal state, and background process activity.
6. Optional stabilizers to record in experiment notes:
   - CPU scaling governor set to performance while measuring
   - fixed ambient load, fixed frequency cap if available
   - rerun from a cold cache baseline if meaningful
7. Optional outputs (when tools exist):
   - `hyperfine --export-json` for run distribution.
   - `perf stat` for hardware counters.
