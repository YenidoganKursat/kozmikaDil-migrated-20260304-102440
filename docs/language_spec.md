# Language Specification (Phase 10 Snapshot)

Date: 2026-02-17

## 1. Lexical and Block Rules
- Source files use UTF-8 text.
- Indentation is significant for block structure.
- Comments: `# ...` to end-of-line.
- Statements are line-oriented.

## 2. Core Syntax
- Assignment: `x = expr`
- Conditionals:
  - `if cond:`
  - `else:`
- Loops:
  - `while cond:`
  - `for v in iterable:`
  - `for i in range(N):`
- Function definitions:
  - `def f(a, b): ...`
  - `fn f(a, b): ...`
  - `async def f(...): ...`
  - `async fn f(...): ...`
- Class definitions:
  - `class Name: ...`
- Arithmetic operators:
  - scalars: `+ - * / %`
  - matrix-matrix `*`: matrix multiplication (`lhs.cols == rhs.rows`)
  - matrix `+ - /` with matrix/scalar: elementwise
  - matrix-scalar `*`: elementwise scale

## 3. Values and Types
- Primitive values:
  - `int` (i64 semantics)
  - `double` (`f64`)
  - `bool`
  - `nil`
- Aggregate/runtime values:
  - `list[T]` / `list[Any]`
  - `matrix[T]` / `matrix[Any]`
  - function and builtin function values
- Surface numeric declarations for extended float families are tracked at type-level design docs; `f64` path is the stabilized perf baseline in current runtime.

## 4. Containers
- List literal: `[1, 2, 3]`
- Matrix literal:
  - canonical row-separator form: `[[1, 2]; [3, 4]]`
  - nested-list style is accepted and normalized
- Indexing:
  - list: `x[i]`
  - matrix: `m[r][c]`, `m[r, c]`
- Slicing:
  - list: `x[a:b]`
  - matrix view-oriented slice patterns: `m[r0:r1, c0:c1]`, `m[:, c]`, `m[r, :]`
- Matrix transpose view: `m.T`

## 5. Builtins and Methods (stabilized subset)
- Scalar/list helpers:
  - `len(x)`, `print(x)`, `range(N)`
  - `accumulate_sum(total, list_or_matrix)` (deterministic running accumulation helper)
  - `append`, `pop`, `insert`, `remove`
  - `map_add`, `map_mul`, `reduce_sum`, `plan_id`, `cache_stats`, `cache_bytes`
- Matrix helpers:
  - `matmul`, `matmul_f32`, `matmul_f64`, `matmul_add`, `matmul_axpby`
  - `matmul_stats`, `matmul_schedule`
  - `matrix_fill_affine`, `matmul_expected_sum`

## 6. Async / Parallel / Event-Driven
- `await expr`
- `spawn fn_or_closure`
- `join(task[, timeout])`
- `with task_group([timeout]) as g: ...`
- `parallel_for`, `par_map`, `par_reduce`
- `channel(capacity)`, `send`, `recv`, `close`
- `stream(channel_like)`, `anext(stream_or_channel)`
- `async for v in stream(...)`

## 7. CLI Surface
- `k parse file.k --dump-ast`
- `k analyze file.k --dump-types --dump-shapes --dump-tier --dump-layout`
- `k run file.k`
- `k build file.k -o out.bin`
- `k --print-cpu-features`

## 8. Compatibility and Evolution Rule
- Syntax changes must preserve parser determinism and include snapshot tests.
- Tier-affecting semantic changes require:
  - updated `docs/tier_model.md`
  - updated diagnostics tests
  - differential check update in Phase 10 gates.
