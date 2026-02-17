#include "../internal_helpers.h"

namespace spark {

Value execute_case_function_def(const FunctionDefStmt& stmt, Interpreter& self,
                               const std::shared_ptr<Environment>& env) {
  auto fn_value = std::make_shared<Value::Function>();
  fn_value->program = nullptr;
  fn_value->body = &stmt.body;
  fn_value->params = stmt.params;
  fn_value->closure = env;
  Value value = Value::function(fn_value);
  if (!env->set(stmt.name, value)) {
    env->define(stmt.name, value);
  }
  return Value::nil();
}

}  // namespace spark
