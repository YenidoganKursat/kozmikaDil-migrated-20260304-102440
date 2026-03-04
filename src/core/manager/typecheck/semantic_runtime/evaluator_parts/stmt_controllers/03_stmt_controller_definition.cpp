#include "../internal_helpers.h"

namespace spark {
namespace {

Value exec_stmt_function_def_controller(const Stmt* stmt, Interpreter& self,
                                        const std::shared_ptr<Environment>& env) {
  return execute_case_function_def(*static_cast<const FunctionDefStmt*>(stmt), self, env);
}

Value exec_stmt_class_def_controller(const Stmt* stmt, Interpreter& self,
                                     const std::shared_ptr<Environment>& env) {
  return execute_case_class_def(*static_cast<const ClassDefStmt*>(stmt), self, env);
}

Value exec_stmt_with_task_group_controller(const Stmt* stmt, Interpreter& self,
                                           const std::shared_ptr<Environment>& env) {
  return execute_case_with_task_group(*static_cast<const WithTaskGroupStmt*>(stmt), self, env);
}

}  // namespace

FastStmtExecFn stmt_exec_controller_definition_for_kind(Stmt::Kind kind) {
  switch (kind) {
    case Stmt::Kind::FunctionDef:
      return &exec_stmt_function_def_controller;
    case Stmt::Kind::ClassDef:
      return &exec_stmt_class_def_controller;
    case Stmt::Kind::WithTaskGroup:
      return &exec_stmt_with_task_group_controller;
    default:
      return nullptr;
  }
}

}  // namespace spark
