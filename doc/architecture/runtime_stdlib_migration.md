# Runtime/Stdlib Canonical Migration Note

As of this migration, production code previously living under repository-root
`runtime/` and `stdlib/` is moved into canonical architecture paths:

- Driver runtime primitives:
  - `include/spark/core/driver/runtime/*`
  - `src/core/driver/runtime/*`
- Logic runtime primitives:
  - `include/spark/core/logic/runtime/*`
  - `src/core/logic/runtime/*`

Hard-cut policy:

- Root `CMakeLists.txt` must not contain `add_subdirectory(runtime)` or
  `add_subdirectory(stdlib)`.
- Legacy production roots `runtime/src`, `runtime/include`,
  `stdlib/src`, `stdlib/include` are forbidden by architecture guard.

This keeps `src/include` as the single canonical production code root.
