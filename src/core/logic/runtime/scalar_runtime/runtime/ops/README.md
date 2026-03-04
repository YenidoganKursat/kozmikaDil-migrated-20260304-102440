# Phase5 Runtime Ops Modules

Single-responsibility modules:

- `ops_common_numeric.cpp`: shared numeric helpers, cache-aware dense access, and dense result builders.
- `ops_list_arithmetic.cpp`: list arithmetic kernels (`+`, `-`, `*`, `/`, `%`).
- `ops_matrix_arithmetic.cpp`: matrix arithmetic kernels (`+`, `-`, `*`, `/`, `%`).
- `runtime_ops.h`: API boundary used by `interpreter_ops.cpp`.
- `controllers/`: operator controller packages + orchestrator (`unary`/`binary` dispatch routing).

`interpreter_ops.cpp` should remain dispatcher-focused and avoid embedding full kernel implementations.
