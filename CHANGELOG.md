# Changelog

## [0.10.0-rc3] - 2026-02-22

### Changed
- Re-ran full CI/CD gate on `master` and recorded complete evidence set before integration finalize.
- Added latest run-log snapshot to `docs/ci_cd_run_log.txt`:
  - local 12/12 test pass + cross-language primitive validation pass,
  - GitHub rerun/dispatch pass set:
    - `CI` run `22268014651` (success),
    - `Workflow Lint` run `22267929371` (success),
    - `Security (CodeQL)` run `22267929376` (success).

### Notes
- `Workflow Lint` and `Security (CodeQL)` are rerun-only in current config (no `workflow_dispatch` trigger).
- `dependency-review` remains PR-only and intentionally out of push/manual branch gate.

## [0.10.0-rc2] - 2026-02-22

### Fixed
- CI flaky failure fixed in float extreme validation:
  - stabilized random case generation by replacing runtime-dependent `hash()` seeding with deterministic seed logic.
  - added guarded handling for low-precision `%` boundary cases that can vary across libc/arch rounding edges.
  - file: `bench/scripts/primitives/validate_float_extreme_bigdecimal.py`.

### Changed
- CI/CD hardening for reliability and diagnostics:
  - migrated CodeQL actions from `v3` to `v4` in `security-codeql.yml`.
  - added apt retry policy (`Acquire::Retries=3`) in all CI workflow install steps.
  - enabled JUnit output emission for CTest runs (core/replay/sanitizer/tsan/nightly) and artifact upload.
  - pinned `reviewdog/action-actionlint` to a full commit SHA in `workflow-lint.yml`.
  - added `.github/dependabot.yml` for weekly GitHub Actions dependency maintenance.

### Validation
- Green runs after hardening:
  - CI: run `22267531705` (success)
  - Security (CodeQL): run `22267531702` (success)
  - Workflow Lint: run `22267531689` (success)

## [0.10.0-rc1] - 2026-02-17

### Added
- Phase 10 portability and release pipeline:
  - multi-arch build gate (`x86_64`, `aarch64`, `riscv64`)
  - CPU feature reporting/dispatch hooks (`--print-cpu-features`)
  - final optimization automation (`scripts/pgo_cycle.sh`, `scripts/bolt_opt.sh`)
  - safety gates (differential, fuzz, sanitizers)
  - phase10 benchmark orchestrator and report output
- New docs/spec set:
  - `docs/language_spec.md`
  - `docs/tier_model.md`
  - `docs/memory_model.md`
  - `docs/container_model.md`
  - `docs/concurrency_model.md`
  - `docs/phase10/*`
- New test target:
  - `sparkc_phase10_tests`

### Changed
- `sparkc` build/run/analyze CLI now supports:
  - `--target`, `--sysroot`
  - `--lto`, `--pgo`, `--pgo-profile`
  - `--dump-layout|--explain-layout`
- Phase8 schedule resolution now reads CPU dispatch hints from centralized feature module.

### Notes
- BOLT stage is optional and automatically reported as skipped when toolchain is unavailable.
