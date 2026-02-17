#include "../internal_helpers.h"

namespace spark {

Value execute_case_function_def(const FunctionDefStmt& stmt, Interpreter& self,
                               const std::shared_ptr<Environment>& env) {
  auto fn_value = std::make_shared<Value::Function>();
  fn_value->program = nullptr;
  fn_value->body = &stmt.body;
  fn_value->params = stmt.params;
  fn_value->is_async = stmt.is_async;
  fn_value->closure = env;
  fn_value->closure_frozen = std::make_shared<Environment>(nullptr, true);
  for (const auto& name : env->keys()) {
    const auto value = env->get(name);
    fn_value->closure_snapshot[name] = value;
    fn_value->closure_frozen->define(name, value);
  }
  Value value = Value::function(fn_value);
  if (!env->set(stmt.name, value)) {
    env->define(stmt.name, value);
  }
  fn_value->closure_snapshot[stmt.name] = value;
  fn_value->closure_frozen->define(stmt.name, value);
  return Value::nil();
}

}  // namespace spark
