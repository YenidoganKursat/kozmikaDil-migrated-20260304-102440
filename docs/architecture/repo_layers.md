# Repository Layer Map

This repository follows a strict layered architecture so each concern is isolated.

## Top-Level Layers

- `compiler/`
  - `include/spark/`: stable public interfaces.
  - `src/common/`: shared utilities used across phases.
  - `src/phase1..phase10/`: phase-scoped implementation units.
- `runtime/`: runtime support libraries and launcher boundaries.
- `stdlib/`: language standard library surface.
- `tests/`
  - `tests/phase1..phase10/`: phase-scoped test suites.
  - Each test target has a phase-specific entrypoint and support module.
- `bench/`
  - `bench/programs/phase*/`: runtime-only benchmark programs.
  - `bench/scripts/`: task-scoped benchmark orchestration scripts.
  - `bench/results/`: generated runtime measurement artifacts.
- `docs/`
  - `docs/phase*/`: phase decisions, reports, constraints.
  - `docs/notes/`: running implementation log and experiment history.

## Separation Rules

1. Shared code must stay in `common` modules; phase folders must not duplicate common helpers.
2. Scripts must be task-focused:
   - one primary benchmark/test task per script.
   - wrappers only delegate, no embedded logic.
3. Test and benchmark outputs are generated artifacts; source files remain deterministic and reviewable.
4. Performance-sensitive changes must document:
   - affected layer,
   - correctness impact,
   - runtime-only measurement evidence.

## Runtime Execution Modes

- `interpret`: direct interpreter execution for correctness/debug.
- `native`: ahead-of-time compiled binary for performance.
- `builtin` benchmark helpers: micro-kernel mode for synthetic stress; not a direct replacement for full-language operator measurements.

