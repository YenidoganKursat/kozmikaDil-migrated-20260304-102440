#include "../internal_helpers.h"

namespace spark {

Value execute_case_while(const WhileStmt& while_stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  Value result = Value::nil();
  while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
    for (const auto& child : while_stmt.body) {
      result = self.execute(*child, env);
    }
  }
  return result;
}

}  // namespace spark
