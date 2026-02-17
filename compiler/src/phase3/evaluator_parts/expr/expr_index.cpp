#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_index(const IndexExpr& index_expr, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  const auto chain = flatten_index_chain(index_expr);
  if (!chain.root) {
    throw EvalException("invalid index expression");
  }
  auto target = self.evaluate(*chain.root, env);
  const ExprEvaluator evaluator = [&self](const Expr& expr, const std::shared_ptr<Environment>& local_env) {
    return self.evaluate(expr, local_env);
  };
  return evaluate_indexed_expression(evaluator, target, chain.indices, env);
}

}  // namespace spark
