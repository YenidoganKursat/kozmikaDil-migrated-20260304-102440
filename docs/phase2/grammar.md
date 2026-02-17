# PHASE 2 Grammar (Minimal Front-End V0)

## Supported statements

```
program      -> statement*
statement    -> class_def | function_def | if_stmt | while_stmt | for_stmt | return_stmt | assignment | expression_stmt
class_def    -> "class" IDENTIFIER [ "(" ( "open" | "slots" ) ")" ] ":" NEWLINE INDENT block
assignment   -> IDENTIFIER "=" expression
expression_stmt -> expression
return_stmt  -> "return" [expression]
if_stmt      -> "if" expression ":" NEWLINE INDENT block
               { "elif" expression ":" NEWLINE INDENT block }*
               [ "else" ":" NEWLINE INDENT block ]
while_stmt   -> "while" expression ":" NEWLINE INDENT block
for_stmt     -> "for" IDENTIFIER "in" expression ":" NEWLINE INDENT block
function_def -> "def" IDENTIFIER "(" [param_list] ")" ":" NEWLINE INDENT block
param_list   -> IDENTIFIER ("," IDENTIFIER)*
block        -> (statement NEWLINE)*
```

## Supported expressions

```
expression   -> or_expr
or_expr      -> and_expr ("or" and_expr)*
and_expr     -> equality_expr ("and" equality_expr)*
equality_expr-> comparison_expr (("==" | "!=") comparison_expr)*
comparison_expr -> term (("<" | "<=" | ">" | ">=") term)*
term         -> factor (("+" | "-") factor)*
factor       -> unary (("*" | "/" | "%") unary)*
unary        -> ("-" | "not") unary | call_or_atom
call_or_atom -> atom ("(" [arg_list] ")" | "[" expression "]" | "." IDENTIFIER)*
arg_list     -> expression ("," expression)*
atom         -> NUMBER | TRUE | FALSE | IDENTIFIER | list_literal | "(" expression ")"
list_literal -> "[" [expression ("," expression)* | list_expr (";" list_expr)*] "]"
```

## Tokens

`IDENTIFIER` supports letters, digits, underscore; must start with letter or underscore.
`NUMBER` supports integer and floating point.
Comments: `#` starts a comment and ignores rest of line.

### Matrix syntax policy

- `[[1, 2], [3, 4]]` is the canonical form.
- `[[1,2];[3,4]]` is also accepted and normalized to canonical nested list form.

### Phase 7 extension note

Method-chain parsing Phase 2 grammar üzerinde kurulu kalır ve Phase 7’de pipeline semantiğiyle çalışır:

- `values.map_add(1).filter_gt(2).reduce_sum()`
