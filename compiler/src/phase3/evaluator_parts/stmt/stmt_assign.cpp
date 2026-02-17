#include "../internal_helpers.h"

namespace spark {

Value execute_case_assign(const AssignStmt& assign, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  auto value = self.evaluate(*assign.value, env);
  if (assign.target->kind == Expr::Kind::Variable) {
    const auto& variable = static_cast<const VariableExpr&>(*assign.target);
    if (!env->set(variable.name, value)) {
      env->define(variable.name, value);
    }
    return Value::nil();
  }

  if (assign.target->kind == Expr::Kind::Index) {
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
