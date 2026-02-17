# Changelog

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
