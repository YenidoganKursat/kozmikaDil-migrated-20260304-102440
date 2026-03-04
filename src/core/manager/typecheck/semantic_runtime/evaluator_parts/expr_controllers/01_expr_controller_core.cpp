#include "../internal_helpers.h"

namespace spark {

namespace {

Value eval_expr_number_controller(const Expr* expr, Interpreter& self,
                                const std::shared_ptr<Environment>& env) {
  return evaluate_case_number(static_cast<const NumberExpr&>(*expr), self, env);
}

Value eval_expr_string_controller(const Expr* expr, Interpreter& self,
                                const std::shared_ptr<Environment>& env) {
  return evaluate_case_string(static_cast<const StringExpr&>(*expr), self, env);
}

Value eval_expr_bool_controller(const Expr* expr, Interpreter& self,
                              const std::shared_ptr<Environment>& env) {
  return evaluate_case_bool(static_cast<const BoolExpr&>(*expr), self, env);
}

Value eval_expr_variable_controller(const Expr* expr, Interpreter& self,
                                  const std::shared_ptr<Environment>& env) {
  return evaluate_case_variable(static_cast<const VariableExpr&>(*expr), self, env);
}

Value eval_expr_list_controller(const Expr* expr, Interpreter& self,
                               const std::shared_ptr<Environment>& env) {
  return evaluate_case_list(static_cast<const ListExpr&>(*expr), self, env);
}

Value eval_expr_unary_controller(const Expr* expr, Interpreter& self,
                                const std::shared_ptr<Environment>& env) {
  return evaluate_case_unary(static_cast<const UnaryExpr&>(*expr), self, env);
}

Value eval_expr_binary_controller(const Expr* expr, Interpreter& self,
                                 const std::shared_ptr<Environment>& env) {
  return evaluate_case_binary(static_cast<const BinaryExpr&>(*expr), self, env);
}

Value eval_expr_call_controller(const Expr* expr, Interpreter& self,
                               const std::shared_ptr<Environment>& env) {
  return evaluate_case_call(static_cast<const CallExpr&>(*expr), self, env);
}

Value eval_expr_attribute_controller(const Expr* expr, Interpreter& self,
                                    const std::shared_ptr<Environment>& env) {
  return evaluate_case_attribute(static_cast<const AttributeExpr&>(*expr), self, env);
}

Value eval_expr_index_controller(const Expr* expr, Interpreter& self,
                                const std::shared_ptr<Environment>& env) {
  return evaluate_case_index(static_cast<const IndexExpr&>(*expr), self, env);
}

}  // namespace

FastExprEvalFn expr_eval_controller_for_kind(Expr::Kind kind) {
  switch (kind) {
    case Expr::Kind::Number:
      return &eval_expr_number_controller;
    case Expr::Kind::String:
      return &eval_expr_string_controller;
    case Expr::Kind::Bool:
      return &eval_expr_bool_controller;
    case Expr::Kind::Variable:
      return &eval_expr_variable_controller;
    case Expr::Kind::List:
      return &eval_expr_list_controller;
    case Expr::Kind::Unary:
      return &eval_expr_unary_controller;
    case Expr::Kind::Binary:
      return &eval_expr_binary_controller;
    case Expr::Kind::Call:
      return &eval_expr_call_controller;
    case Expr::Kind::Attribute:
      return &eval_expr_attribute_controller;
    case Expr::Kind::Index:
      return &eval_expr_index_controller;
  }

  return nullptr;
}

}  // namespace spark
