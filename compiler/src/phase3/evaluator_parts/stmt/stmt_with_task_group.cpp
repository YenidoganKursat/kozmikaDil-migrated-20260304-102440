#include "../internal_helpers.h"

namespace spark {

Value execute_case_with_task_group(const WithTaskGroupStmt& stmt, Interpreter& self,
                                  const std::shared_ptr<Environment>& env) {
  std::optional<long long> timeout_ms = std::nullopt;
  if (stmt.timeout_ms) {
    const auto value = self.evaluate(*stmt.timeout_ms, env);
    timeout_ms = value_to_int(value);
    if (*timeout_ms < 0) {
      throw EvalException("with task_group timeout must be non-negative");
    }
  }

  auto group = make_task_group_value(timeout_ms);
  if (!env->set(stmt.name, group)) {
    env->define(stmt.name, group);
  }

  Value result = Value::nil();
  try {
    for (const auto& child : stmt.body) {
      result = self.execute(*child, env);
    }
    auto* live_group = env->get_ptr(stmt.name);
    if (live_group && live_group->kind == Value::Kind::TaskGroup) {
      (void)task_group_join_all_value(*live_group);
    }
    return result;
  } catch (...) {
    auto* live_group = env->get_ptr(stmt.name);
    if (live_group && live_group->kind == Value::Kind::TaskGroup) {
      (void)task_group_cancel_all_value(*live_group);
      try {
        (void)task_group_join_all_value(*live_group);
      } catch (...) {
      }
    }
    throw;
  }
}

}  // namespace spark
