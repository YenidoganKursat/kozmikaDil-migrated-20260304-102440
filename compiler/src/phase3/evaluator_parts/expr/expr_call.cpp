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

  // Fast-path numeric primitive constructors.
  // This avoids generic builtin dispatch overhead in hot loops while preserving
  // full literal precision for NumberExpr source text.
  if (callee.kind == Value::Kind::Builtin && callee.builtin_value && call.args.size() == 1) {
    bool is_numeric_constructor = false;
    Value::NumericKind target_kind = Value::NumericKind::F64;
    try {
      target_kind = numeric_kind_from_name(callee.builtin_value->name);
      is_numeric_constructor = true;
    } catch (const EvalException&) {
      is_numeric_constructor = false;
    }

    if (is_numeric_constructor) {
      if (call.args[0] && call.args[0]->kind == Expr::Kind::Number) {
        const auto& number = static_cast<const NumberExpr&>(*call.args[0]);
        if (!number.raw_text.empty()) {
          const auto source_kind = number.is_int ? Value::NumericKind::I512 : Value::NumericKind::F512;
          const auto source = Value::numeric_value_of(source_kind, number.raw_text);
          return cast_numeric_to_kind(target_kind, source);
        }
      }
      const auto value = self.evaluate(*call.args[0], env);
      if (!is_numeric_kind(value)) {
        throw EvalException(callee.builtin_value->name + "() expects exactly one numeric argument");
      }
      return cast_numeric_to_kind(target_kind, value);
    }
  }

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
