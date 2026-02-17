#include "../internal_helpers.h"

namespace spark {

Value execute_case_if(const IfStmt& if_stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env) {
  if (self.truthy(self.evaluate(*if_stmt.condition, env))) {
    for (const auto& child : if_stmt.then_body) {
      self.execute(*child, env);
    }
    return Value::nil();
  }
  for (const auto& branch : if_stmt.elif_branches) {
    if (self.truthy(self.evaluate(*branch.first, env))) {
      for (const auto& child : branch.second) {
        self.execute(*child, env);
      }
      return Value::nil();
    }
  }
  for (const auto& child : if_stmt.else_body) {
    self.execute(*child, env);
  }
  return Value::nil();
}

}  // namespace spark
