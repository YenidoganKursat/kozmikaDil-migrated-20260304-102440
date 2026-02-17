#include <vector>

#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_call(const CallExpr& call, Interpreter& self,
                        const std::shared_ptr<Environment>& env) {
  Value pipeline_result;
  if (try_execute_pipeline_call(call, self, env, pipeline_result)) {
    return pipeline_result;
  }

  auto callee = self.evaluate(*call.callee, env);
  std::vector<Value> args;
  args.reserve(call.args.size());
  for (const auto& arg : call.args) {
    args.push_back(self.evaluate(*arg, env));
  }
  if (callee.kind == Value::Kind::Function && callee.function_value &&
      callee.function_value->is_async) {
    return spawn_task_value(callee, args);
  }
  return invoke_callable_sync(callee, args);
}

}  // namespace spark
