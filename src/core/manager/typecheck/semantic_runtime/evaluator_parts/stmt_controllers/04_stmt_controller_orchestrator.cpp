#include "../internal_helpers.h"

namespace spark {

// Leaf controller lookups (implemented in sibling modules).
FastStmtExecFn stmt_exec_controller_core_for_kind(Stmt::Kind kind);
FastStmtExecFn stmt_exec_controller_control_flow_for_kind(Stmt::Kind kind);
FastStmtExecFn stmt_exec_controller_definition_for_kind(Stmt::Kind kind);

FastStmtExecFn stmt_exec_controller_for_kind(Stmt::Kind kind) {
  if (const auto fn = stmt_exec_controller_core_for_kind(kind); fn != nullptr) {
    return fn;
  }
  if (const auto fn = stmt_exec_controller_control_flow_for_kind(kind); fn != nullptr) {
    return fn;
  }
  return stmt_exec_controller_definition_for_kind(kind);
}

}  // namespace spark
