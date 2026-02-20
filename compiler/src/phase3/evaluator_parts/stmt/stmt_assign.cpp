#include <cstdlib>
#include <string>

#include "../internal_helpers.h"

namespace spark {

namespace {

bool env_bool_enabled_assign(const char* name, bool fallback) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  const std::string text = value;
  if (text == "0" || text == "false" || text == "False" || text == "off" || text == "OFF" ||
      text == "no" || text == "NO") {
    return false;
  }
  return true;
}

bool is_numeric_arithmetic_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

}  // namespace

Value execute_case_assign(const AssignStmt& assign, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  if (assign.target->kind == Expr::Kind::Variable) {
    const auto& variable = static_cast<const VariableExpr&>(*assign.target);
    const bool inplace_numeric_assign =
        env_bool_enabled_assign("SPARK_ASSIGN_INPLACE_NUMERIC", true);
    if (auto* current = env->get_ptr(variable.name); current != nullptr) {
      if (inplace_numeric_assign && assign.value && assign.value->kind == Expr::Kind::Binary) {
        const auto& binary = static_cast<const BinaryExpr&>(*assign.value);
        if (is_numeric_arithmetic_op(binary.op)) {
          const auto left = self.evaluate(*binary.left, env);
          const auto right = self.evaluate(*binary.right, env);
          if (eval_numeric_binary_value_inplace(binary.op, left, right, *current)) {
            return Value::nil();
          }
          *current = self.eval_binary(binary.op, left, right);
          return Value::nil();
        }
      }
      *current = self.evaluate(*assign.value, env);
      return Value::nil();
    }
    auto value = self.evaluate(*assign.value, env);
    env->define(variable.name, value);
    return Value::nil();
  }

  if (assign.target->kind == Expr::Kind::Index) {
    auto value = self.evaluate(*assign.value, env);
    const auto target = flatten_index_target(*assign.target);
    if (!target.variable) {
      throw EvalException("invalid assignment target");
    }
    auto current = env->get(target.variable->name);
    const ExprEvaluator evaluator = [&self](const Expr& expr, const std::shared_ptr<Environment>& local_env) {
      return self.evaluate(expr, local_env);
    };
    assign_indexed_expression(evaluator, current, target.indices, 0, env, value);
    if (!env->set(target.variable->name, current)) {
      throw EvalException("cannot assign to undefined variable: " + target.variable->name);
    }
    return Value::nil();
  }
  throw EvalException("invalid assignment target");
}

}  // namespace spark
