# Ops Controllers Package

`containers_primitives/runtime/ops/controllers` splits binary/unary operator handling into modular controller packages:

- `01_ops_controller_core.cpp`
  - logical ops (`and`, `or`)
  - numeric fast paths
  - `==` / `!=` generic equality
- `02_ops_controller_containers.cpp`
  - `string`, `list`, `matrix` operator controllers
- `03_ops_controller_scalar.cpp`
  - scalar arithmetic/comparison controllers (`+ - * / % ^ < <= > >=`)
- `04_ops_controller_orchestrator.cpp`
  - single orchestration entrypoint that chains all controller packages

`Interpreter::eval_unary` and `Interpreter::eval_binary` should remain thin wrappers that delegate into this orchestrator.
