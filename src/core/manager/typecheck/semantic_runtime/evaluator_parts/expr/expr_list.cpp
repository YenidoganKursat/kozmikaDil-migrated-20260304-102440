#include <vector>

#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_list(const ListExpr& list, Interpreter& self,
                        const std::shared_ptr<Environment>& env) {
  ExprEvaluator evaluator = [&self](const Expr& inner, const std::shared_ptr<Environment>& local_env) {
    return self.evaluate(inner, local_env);
  };

  std::vector<Value> values;
  values.reserve(list.elements.size());
  for (const auto& element : list.elements) {
    values.push_back(self.evaluate(*element, env));
  }
  const auto matrix = evaluate_as_matrix_literal(evaluator, list, env);
  if (matrix.has_value()) {
    return *matrix;
  }
  return Value::list_value_of(std::move(values));
}

}  // namespace spark
