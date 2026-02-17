# Phase 2 Syntax (Canonical)

The language uses Python-like indentation for blocks and Python-flavored statement syntax.

## 1) Statements

```ebnf
program        -> statement*
statement      -> class_def | function_def | async_function_def | with_task_group_stmt
               | if_stmt | while_stmt | for_stmt | async_for_stmt
               | return_stmt | assignment | expression_stmt
class_def      -> "class" IDENTIFIER [ "(" ( "open" | "slots" ) ")" ] ":" NEWLINE INDENT block
function_def   -> "def" IDENTIFIER "(" [param_list] ")" ":" NEWLINE INDENT block
async_function_def -> ("async def" | "async fn") IDENTIFIER "(" [param_list] ")" ":" NEWLINE INDENT block
with_task_group_stmt -> "with task_group" ["(" expression ")"] "as" IDENTIFIER ":" NEWLINE INDENT block
param_list     -> IDENTIFIER ("," IDENTIFIER)*
return_stmt    -> "return" [expression]
if_stmt        -> "if" expression ":" NEWLINE INDENT block {"elif" expression ":" NEWLINE INDENT block} ["else" ":" NEWLINE INDENT block]
while_stmt     -> "while" expression ":" NEWLINE INDENT block
for_stmt       -> "for" IDENTIFIER "in" expression ":" NEWLINE INDENT block
async_for_stmt -> "async for" IDENTIFIER "in" expression ":" NEWLINE INDENT block
assignment     -> IDENTIFIER "=" expression
expression_stmt -> expression
block          -> (statement NEWLINE)*
```

## 2) Expressions

```ebnf
expression     -> or_expr
or_expr        -> and_expr ("or" and_expr)*
and_expr       -> equality_expr ("and" equality_expr)*
equality_expr  -> comparison_expr (("==" | "!=") comparison_expr)*
comparison_expr-> term (("<" | "<=" | ">" | ">=") term)*
term           -> factor (("+" | "-") factor)*
factor         -> unary (("*" | "/" | "%") unary)*
unary          -> ("-" | "not" | "await") unary | call_or_atom
call_or_atom   -> atom (call_or_index)*
call_or_index  -> "(" [arg_list] ")" | "[" expression "]" | "." IDENTIFIER
arg_list       -> expression ("," expression)*
atom           -> IDENTIFIER | NUMBER | TRUE | FALSE | list_literal | "(" expression ")"
list_literal   -> "[" list_items "]"
list_items     -> [ expression_list | matrix_rows ]
expression_list-> expression ("," expression)*
matrix_rows    -> list_expr (";" list_expr)*
```

### Matrix syntax policy

- Parser accepts both styles:
  - Python-style: `[[1,2],[3,4]]`
  - Semicolon-style row separator (legacy): `[[1,2];[3,4]]`
- Both are normalized into the same AST form (`ListExpr` where each row is itself a `ListExpr`), so parser ambiguity is gone and downstream tools can rely on one canonical representation.

### Lexing / tokens

- `IDENTIFIER`: `[A-Za-z_][A-Za-z0-9_]*`
- `NUMBER`: integer and float literals
- `TRUE` / `FALSE`
- unary keywords: `not`, `await`
- comments: `# ...` till end of line

### Concurrency Runtime Builtins (Phase 9)

- Task primitives:
  - `spawn(fn, ...)`
  - `join(task [, timeout_ms])`
  - `deadline(timeout_ms)` (timeout/deadline alias helper)
  - `cancel(task)`
  - `task_group([timeout_ms])`
- Parallel primitives:
  - `parallel_for(start, stop, fn [, extra...])`
  - `par_map(list, fn)`
  - `par_reduce(list, init, fn)`
- Event-driven primitives:
  - `channel([capacity])`, `send(ch, v)`, `recv(ch [, timeout_ms])`, `close(ch)`
  - `stream(ch)`, `anext(ch_or_stream [, timeout_ms])`
  - channel methods: `.send()`, `.recv()`, `.anext()`, `.has_next()`, `.close()`, `.stats()`

### Phase 7 method-chain note

Phase 7 ile birlikte method-chain pipeline biçimi semantik olarak aktiftir:

- `x.map_add(1).filter_gt(2).reduce_sum()`
- `x.map_add(1).scan_sum()`
- `m.map_mul(2).reduce_sum()`
