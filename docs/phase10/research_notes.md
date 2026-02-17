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
