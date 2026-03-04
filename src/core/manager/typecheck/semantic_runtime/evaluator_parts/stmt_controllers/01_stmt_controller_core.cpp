#include "../internal_helpers.h"

namespace spark {
namespace {

Value exec_stmt_expression_controller(const Stmt* stmt, Interpreter& self,
                                      const std::shared_ptr<Environment>& env) {
  return execute_case_expression(*static_cast<const ExpressionStmt*>(stmt), self, env);
}

Value exec_stmt_assign_controller(const Stmt* stmt, Interpreter& self,
                                  const std::shared_ptr<Environment>& env) {
  return execute_case_assign(*static_cast<const AssignStmt*>(stmt), self, env);
}

Value exec_stmt_return_controller(const Stmt* stmt, Interpreter& self,
                                  const std::shared_ptr<Environment>& env) {
  return execute_case_return(*static_cast<const ReturnStmt*>(stmt), self, env);
}

Value exec_stmt_break_controller(const Stmt* stmt, Interpreter& self,
                                 const std::shared_ptr<Environment>& env) {
  return execute_case_break(*static_cast<const BreakStmt*>(stmt), self, env);
}

Value exec_stmt_continue_controller(const Stmt* stmt, Interpreter& self,
                                    const std::shared_ptr<Environment>& env) {
  return execute_case_continue(*static_cast<const ContinueStmt*>(stmt), self, env);
}

}  // namespace

FastStmtExecFn stmt_exec_controller_core_for_kind(Stmt::Kind kind) {
  switch (kind) {
    case Stmt::Kind::Expression:
      return &exec_stmt_expression_controller;
    case Stmt::Kind::Assign:
      return &exec_stmt_assign_controller;
    case Stmt::Kind::Return:
      return &exec_stmt_return_controller;
    case Stmt::Kind::Break:
      return &exec_stmt_break_controller;
    case Stmt::Kind::Continue:
      return &exec_stmt_continue_controller;
    default:
      return nullptr;
  }
}

}  // namespace spark
