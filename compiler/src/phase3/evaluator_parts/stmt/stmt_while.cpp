#include "../internal_helpers.h"

namespace spark {

Value execute_case_while(const WhileStmt& while_stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  const auto& body = while_stmt.body;
  if (body.empty()) {
    while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
    }
    return Value::nil();
  }

  Value result = Value::nil();
  if (body.size() == 1) {
    const auto& only = *body.front();
    while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
      result = self.execute(only, env);
    }
    return result;
  }

  while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
    for (const auto& child : body) {
      result = self.execute(*child, env);
    }
  }
  return result;
}

}  // namespace spark
