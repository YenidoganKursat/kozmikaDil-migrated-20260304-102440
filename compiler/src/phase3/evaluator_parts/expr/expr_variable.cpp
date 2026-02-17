#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_variable(const VariableExpr& expr, Interpreter&,
                            const std::shared_ptr<Environment>& env) {
  return env->get(expr.name);
}

}  // namespace spark
