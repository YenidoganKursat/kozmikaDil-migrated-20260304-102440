#include "internal_helpers.h"

namespace spark {

Value Interpreter::evaluate(const Expr& expr, const std::shared_ptr<Environment>& env) {
  switch (expr.kind) {
    case Expr::Kind::Number:
      return evaluate_case_number(static_cast<const NumberExpr&>(expr), *this, env);
    case Expr::Kind::Bool:
      return evaluate_case_bool(static_cast<const BoolExpr&>(expr), *this, env);
    case Expr::Kind::Variable:
      return evaluate_case_variable(static_cast<const VariableExpr&>(expr), *this, env);
    case Expr::Kind::List:
      return evaluate_case_list(static_cast<const ListExpr&>(expr), *this, env);
    case Expr::Kind::Unary:
      return evaluate_case_unary(static_cast<const UnaryExpr&>(expr), *this, env);
    case Expr::Kind::Binary:
      return evaluate_case_binary(static_cast<const BinaryExpr&>(expr), *this, env);
    case Expr::Kind::Call:
      return evaluate_case_call(static_cast<const CallExpr&>(expr), *this, env);
    case Expr::Kind::Attribute:
      return evaluate_case_attribute(static_cast<const AttributeExpr&>(expr), *this, env);
    case Expr::Kind::Index:
      return evaluate_case_index(static_cast<const IndexExpr&>(expr), *this, env);
  }

  throw EvalException("unsupported expression");
}

Value Interpreter::execute(const Stmt& stmt, const std::shared_ptr<Environment>& env) {
  switch (stmt.kind) {
    case Stmt::Kind::Expression:
      return execute_case_expression(static_cast<const ExpressionStmt&>(stmt), *this, env);
    case Stmt::Kind::Assign:
      return execute_case_assign(static_cast<const AssignStmt&>(stmt), *this, env);
    case Stmt::Kind::Return:
      return execute_case_return(static_cast<const ReturnStmt&>(stmt), *this, env);
    case Stmt::Kind::If:
      return execute_case_if(static_cast<const IfStmt&>(stmt), *this, env);
    case Stmt::Kind::While:
      return execute_case_while(static_cast<const WhileStmt&>(stmt), *this, env);
    case Stmt::Kind::For:
      return execute_case_for(static_cast<const ForStmt&>(stmt), *this, env);
    case Stmt::Kind::FunctionDef:
      return execute_case_function_def(static_cast<const FunctionDefStmt&>(stmt), *this, env);
    case Stmt::Kind::ClassDef:
      return execute_case_class_def(static_cast<const ClassDefStmt&>(stmt), *this, env);
    case Stmt::Kind::WithTaskGroup:
      return execute_case_with_task_group(static_cast<const WithTaskGroupStmt&>(stmt), *this, env);
  }

  throw EvalException("unsupported statement");
}

}  // namespace spark
