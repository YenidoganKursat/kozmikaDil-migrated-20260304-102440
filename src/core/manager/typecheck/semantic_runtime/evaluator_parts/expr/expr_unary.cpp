#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_unary(const UnaryExpr& unary, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  return self.eval_unary(unary.op, self.evaluate(*unary.operand, env));
}

}  // namespace spark
