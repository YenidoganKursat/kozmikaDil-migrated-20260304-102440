# Current Status

Updated: 2026-03-01

## Applied repository organization

1. Architecture roots moved under `src/`:
   - `src/core`
   - `src/port`
   - `src/application`
2. Tests root normalized as `test/` with compatibility symlink:
   - `tests -> test`
3. All documentation root moved under `doc/`.
4. Compatibility symlink kept:
   - `docs -> doc`
5. Benchmark/result records collected under `record/bench_results`.
6. Compatibility symlink kept:
   - `bench/results -> ../record/bench_results`
7. Legacy CLI bridge removed previously:
   - removed `legacy_cli_entry`
   - active entrypoint uses `compiler_entry`
8. Build/CI canonical paths migrated:
   - `CMakeLists.txt`: `add_subdirectory(test)`
   - `compiler/CMakeLists.txt`: `SPARK_TEST_SOURCE_DIR=${CMAKE_SOURCE_DIR}/test`
   - CI workflows now call `./test/...` and `doc/platform_support_policy.json`
9. Cleanup artifacts moved under:
   - `record/_trash`

## Architecture compliance status

- Guard check result: `errors=0 warnings=0`
- Command used:
  - `python3 .github/scripts/architecture_guard.py`

## What remains outside the pure core/port/application surface

These still exist intentionally as phase implementation backends and are bridged through approved core files:

1. `compiler/src/phase1`
2. `compiler/src/phase2`
3. `compiler/src/phase3`
4. `compiler/src/phase4`
5. `compiler/src/phase5`
6. `compiler/src/phase6`
7. `compiler/src/phase7`
8. `compiler/src/phase8`
9. `compiler/src/phase9`
10. `test/core/pipeline/phase1..phase10`
11. `scripts/core/pipeline/phase10`

Notes:

1. These are not active architecture violations under current rules.
2. They are treated as implementation modules behind core bridges.
