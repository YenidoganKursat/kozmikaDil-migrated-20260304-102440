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
  if (callee.kind == Value::Kind::Builtin) {
    return callee.builtin_value->impl(args);
  }
  if (callee.kind == Value::Kind::Function) {
    const auto& fn = callee.function_value;
    if (!fn || !fn->body) {
      throw EvalException("invalid function value");
    }
    if (fn->params.size() != args.size()) {
      throw EvalException("function argument count mismatch");
    }
    auto local_env = std::make_shared<Environment>(fn->closure);
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
      local_env->define(fn->params[i], args[i]);
    }
    Value result = Value::nil();
    try {
      for (const auto& stmt : *fn->body) {
        result = self.execute(*stmt, local_env);
      }
    } catch (const Interpreter::ReturnSignal& signal) {
      return signal.value;
    }
    return result;
  }
  throw EvalException("attempted to call non-callable value");
}

}  // namespace spark
