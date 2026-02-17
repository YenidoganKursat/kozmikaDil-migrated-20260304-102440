#include "../internal_helpers.h"

namespace spark {

Value execute_case_expression(const ExpressionStmt& stmt, Interpreter& self,
                             const std::shared_ptr<Environment>& env) {
  return self.evaluate(*stmt.expression, env);
}

}  // namespace spark
