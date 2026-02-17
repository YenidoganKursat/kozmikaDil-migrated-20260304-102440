#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_binary(const BinaryExpr& binary, Interpreter& self,
                          const std::shared_ptr<Environment>& env) {
  auto lhs = self.evaluate(*binary.left, env);
  auto rhs = self.evaluate(*binary.right, env);
  return self.eval_binary(binary.op, lhs, rhs);
}

}  // namespace spark
