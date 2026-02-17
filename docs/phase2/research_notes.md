# Research Notes (Phase 2)

## Practical decisions for parser + evaluator

- Pratt vs precedence-climbing parsing:
  - Implemented precedence climbing for expression parsing with explicit precedence table.
  - Keeps expression grammar compact and makes extension (e.g., unary precedence, bitwise ops) easier.
- AST first, direct lowering later:
  - Using a concrete AST allows later IR conversion without freezing syntax or runtime semantics.
- Minimal evaluator as executable correctness oracle:
  - Small interpreter enables deterministic behavior checks before MLIR/LLVM integration.
  - Prevents optimizer-driven regressions before frontend is stable.
- List-centric semantics in phase2:
- List literal support (`[ ... ]`) and indexing provide the first concrete IR target for future container pipeline work.
- Indentation-based blocks:
  - Python-like layout is sufficient for tiny language prototype and keeps syntax readable.

