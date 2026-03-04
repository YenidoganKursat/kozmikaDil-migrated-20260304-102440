#include "../internal_helpers.h"

namespace spark {
namespace {

Value exec_stmt_if_controller(const Stmt* stmt, Interpreter& self,
                              const std::shared_ptr<Environment>& env) {
  return execute_case_if(*static_cast<const IfStmt*>(stmt), self, env);
}

Value exec_stmt_switch_controller(const Stmt* stmt, Interpreter& self,
                                  const std::shared_ptr<Environment>& env) {
  return execute_case_switch(*static_cast<const SwitchStmt*>(stmt), self, env);
}

Value exec_stmt_try_catch_controller(const Stmt* stmt, Interpreter& self,
                                     const std::shared_ptr<Environment>& env) {
  return execute_case_try_catch(*static_cast<const TryCatchStmt*>(stmt), self, env);
}

Value exec_stmt_while_controller(const Stmt* stmt, Interpreter& self,
                                 const std::shared_ptr<Environment>& env) {
  return execute_case_while(*static_cast<const WhileStmt*>(stmt), self, env);
}

Value exec_stmt_for_controller(const Stmt* stmt, Interpreter& self,
                               const std::shared_ptr<Environment>& env) {
  return execute_case_for(*static_cast<const ForStmt*>(stmt), self, env);
}

}  // namespace

FastStmtExecFn stmt_exec_controller_control_flow_for_kind(Stmt::Kind kind) {
  switch (kind) {
    case Stmt::Kind::If:
      return &exec_stmt_if_controller;
    case Stmt::Kind::Switch:
      return &exec_stmt_switch_controller;
    case Stmt::Kind::TryCatch:
      return &exec_stmt_try_catch_controller;
    case Stmt::Kind::While:
      return &exec_stmt_while_controller;
    case Stmt::Kind::For:
      return &exec_stmt_for_controller;
    default:
      return nullptr;
  }
}

}  // namespace spark
