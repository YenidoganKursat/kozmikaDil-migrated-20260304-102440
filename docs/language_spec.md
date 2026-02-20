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
  - scalars: `+ - * / % ^`
  - matrix-matrix `*`: matrix multiplication (`lhs.cols == rhs.rows`)
  - matrix `+ - / % ^` with matrix/scalar: elementwise
  - matrix-scalar `*`: elementwise scale (matrix-matrix `*` matmul semantics)
  - heterogeneous list/matrix cells (string/object) support dynamic fallback for `+`
    and string-repeat style `*`; other arithmetic operators require numeric cells.

## 3. Values and Types
- Primitive values:
  - `int` (i64 semantics)
  - `double` (`f64`)
  - numeric primitive constructors (runtime scalar wrappers):
    - int family: `i8(x)`, `i16(x)`, `i32(x)`, `i64(x)`, `i128(x)`, `i256(x)`, `i512(x)`
    - float family: `f8(x)`, `f16(x)`, `bf16(x)`, `f32(x)`, `f64(x)`, `f128(x)`, `f256(x)`, `f512(x)`
  - constructor sugar (single-argument prefix form):
    - `x = f512 123.45` (canonicalized to `f512(123.45)`)
    - same support for `f8/f16/f32/f64/f128/f256/f512`, int family constructors, and `string`
  - string primitive:
    - literal: `"hello"` or `'hello'`
    - constructor: `string(x)` and prefix sugar `string "hello"`
    - length semantics:
      - `len(string)` -> Unicode codepoint count
      - `utf8_len(string)` -> UTF-8 byte length
      - `utf16_len(string)` -> UTF-16 code-unit length
  - `bool`
  - `nil`
- Aggregate/runtime values:
  - `list[T]` / `list[Any]`
  - `matrix[T]` / `matrix[Any]`
  - function and builtin function values
- Surface numeric declarations for extended float families are tracked at type-level design docs; `f64` path is the stabilized perf baseline in current runtime.
- Current fallback backend keeps the full primitive surface active (`i8..i512`, `f8..f512`), while true arbitrary-precision execution for `i256/i512` and `f128+` is planned as an optional multiprecision backend.
- High-precision float execution (`f128/f256/f512`) now uses MPFR-backed interpreter runtime path for correct semantics.
- `k run` automatically switches to interpreter mode when `f128/f256/f512` is detected, so native C backend does not silently downcast these types.
- High-precision strictness policy:
  - `f128/f256/f512` remain interpreter/MPFR-backed for correctness.
  - `SPARK_ALLOW_APPROX_HIGH_PRECISION_NATIVE` is deprecated/ignored in strict correctness mode.

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
  - `len(x)`, `print(x)`, `range(N)`, `string(x)`, `utf8_len(s)`, `utf16_len(s)`
  - `bench_tick()` (monotonic-ish runtime tick for benchmark seeding)
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
