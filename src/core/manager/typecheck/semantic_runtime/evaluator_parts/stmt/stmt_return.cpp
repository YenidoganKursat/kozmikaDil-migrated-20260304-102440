#include "../internal_helpers.h"

namespace spark {

Value execute_case_return(const ReturnStmt& stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  Value value = Value::nil();
  if (stmt.value) {
    value = self.evaluate(*stmt.value, env);
  }
  throw Interpreter::ReturnSignal{value};
}

}  // namespace spark
