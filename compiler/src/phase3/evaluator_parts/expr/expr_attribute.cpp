#include "../internal_helpers.h"

namespace spark {

Value evaluate_case_attribute(const AttributeExpr& attribute, Interpreter& self,
                             const std::shared_ptr<Environment>& env) {
  const auto target_is_variable = attribute.target->kind == Expr::Kind::Variable;
  const std::string target_name = target_is_variable
                                       ? static_cast<const VariableExpr&>(*attribute.target).name
                                       : "";
  const std::optional<Value> captured_target =
      target_is_variable ? std::nullopt : std::optional<Value>(self.evaluate(*attribute.target, env));

  auto make_list_value_builtin = [env, target_name](const std::string& method,
                                                    std::size_t expected, auto handler) {
    return Value::builtin(method, [env, target_name, expected, method, handler](const std::vector<Value>& args) -> Value {
      if (args.size() != expected) {
        throw EvalException(method + " expects exactly " + std::to_string(expected) + " arguments");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind != Value::Kind::List) {
        throw EvalException(method + "() only supported on list variables");
      }
      return handler(*current, args);
    });
  };

  auto make_matrix_value_builtin = [env, target_is_variable, target_name, captured_target](
                                       const std::string& method, std::size_t expected, auto handler) {
    return Value::builtin(method, [env, target_is_variable, target_name, captured_target, expected, method, handler](
                                      const std::vector<Value>& args) -> Value {
      if (args.size() != expected) {
        throw EvalException(method + " expects exactly " + std::to_string(expected) + " arguments");
      }

      Value local;
      Value* current = nullptr;
      if (target_is_variable) {
        current = env->get_ptr(target_name);
      } else if (captured_target.has_value()) {
        local = *captured_target;
        current = &local;
      }

      if (!current) {
        throw EvalException("undefined value for method receiver");
      }
      if (current->kind != Value::Kind::Matrix || !current->matrix_value) {
        throw EvalException(method + "() only supported on matrix values");
      }
      return handler(*current, args);
    });
  };

  auto make_task_group_builtin = [env, target_is_variable, target_name, captured_target](
                                     const std::string& method, std::size_t expected, auto handler) {
    return Value::builtin(method, [env, target_is_variable, target_name, captured_target, expected, method, handler](
                                      const std::vector<Value>& args) -> Value {
      if (args.size() != expected && expected != static_cast<std::size_t>(-1)) {
        throw EvalException(method + " expects exactly " + std::to_string(expected) + " arguments");
      }

      Value local;
      Value* current = nullptr;
      if (target_is_variable) {
        current = env->get_ptr(target_name);
      } else if (captured_target.has_value()) {
        local = *captured_target;
        current = &local;
      }
      if (!current || current->kind != Value::Kind::TaskGroup || !current->task_group_value) {
        throw EvalException(method + "() only supported on task_group values");
      }
      return handler(*current, args);
    });
  };

  auto make_task_builtin = [env, target_is_variable, target_name, captured_target](
                               const std::string& method, std::size_t expected, auto handler) {
    return Value::builtin(method, [env, target_is_variable, target_name, captured_target, expected, method, handler](
                                      const std::vector<Value>& args) -> Value {
      if (args.size() != expected && expected != static_cast<std::size_t>(-1)) {
        throw EvalException(method + " expects exactly " + std::to_string(expected) + " arguments");
      }

      Value local;
      Value* current = nullptr;
      if (target_is_variable) {
        current = env->get_ptr(target_name);
      } else if (captured_target.has_value()) {
        local = *captured_target;
        current = &local;
      }
      if (!current || current->kind != Value::Kind::Task || !current->task_value) {
        throw EvalException(method + "() only supported on task values");
      }
      return handler(*current, args);
    });
  };

  auto make_channel_builtin = [env, target_is_variable, target_name, captured_target](
                                  const std::string& method, std::size_t expected, auto handler) {
    return Value::builtin(method, [env, target_is_variable, target_name, captured_target, expected, method, handler](
                                      const std::vector<Value>& args) -> Value {
      if (args.size() != expected && expected != static_cast<std::size_t>(-1)) {
        throw EvalException(method + " expects exactly " + std::to_string(expected) + " arguments");
      }

      Value local;
      Value* current = nullptr;
      if (target_is_variable) {
        current = env->get_ptr(target_name);
      } else if (captured_target.has_value()) {
        local = *captured_target;
        current = &local;
      }
      if (!current || current->kind != Value::Kind::Channel || !current->channel_value) {
        throw EvalException(method + "() only supported on channel values");
      }
      return handler(*current, args);
    });
  };

  if (attribute.attribute == "T" || attribute.attribute == "transpose") {
    auto value = self.evaluate(*attribute.target, env);
    if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
      throw EvalException("transpose only supports matrix values");
    }
    return transpose_matrix(value);
  }

  if (attribute.attribute == "matmul") {
    return make_matrix_value_builtin("matmul", 1, [](Value& current, const std::vector<Value>& args) {
      return matrix_matmul_value(current, args[0]);
    });
  }

  if (attribute.attribute == "matmul_f32") {
    return make_matrix_value_builtin("matmul_f32", 1, [](Value& current, const std::vector<Value>& args) {
      return matrix_matmul_f32_value(current, args[0]);
    });
  }

  if (attribute.attribute == "matmul_f64") {
    return make_matrix_value_builtin("matmul_f64", 1, [](Value& current, const std::vector<Value>& args) {
      return matrix_matmul_f64_value(current, args[0]);
    });
  }

  if (attribute.attribute == "matmul_add") {
    return make_matrix_value_builtin("matmul_add", 2, [](Value& current, const std::vector<Value>& args) {
      return matrix_matmul_add_value(current, args[0], args[1]);
    });
  }

  if (attribute.attribute == "matmul_axpby") {
    return make_matrix_value_builtin("matmul_axpby", 4, [](Value& current, const std::vector<Value>& args) {
      return matrix_matmul_axpby_value(current, args[0], args[1], args[2], args[3]);
    });
  }

  if (attribute.attribute == "matmul_stats") {
    return make_matrix_value_builtin("matmul_stats", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return matrix_matmul_stats_value(current);
    });
  }

  if (attribute.attribute == "matmul_schedule") {
    return make_matrix_value_builtin("matmul_schedule", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return matrix_matmul_schedule_value(current);
    });
  }

  if (attribute.attribute == "spawn") {
    return make_task_group_builtin("spawn", static_cast<std::size_t>(-1),
                                   [](Value& current, const std::vector<Value>& args) {
      if (args.empty()) {
        throw EvalException("task_group.spawn() expects at least callable argument");
      }
      std::vector<Value> task_args;
      task_args.reserve(args.size() - 1);
      for (std::size_t i = 1; i < args.size(); ++i) {
        task_args.push_back(args[i]);
      }
      return task_group_spawn_value(current, args[0], task_args);
    });
  }

  if (attribute.attribute == "join_all") {
    return make_task_group_builtin("join_all", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return task_group_join_all_value(current);
    });
  }

  if (attribute.attribute == "cancel_all") {
    return make_task_group_builtin("cancel_all", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return task_group_cancel_all_value(current);
    });
  }

  if (attribute.attribute == "join") {
    return make_task_builtin("join", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return await_task_value(current);
    });
  }

  if (attribute.attribute == "cancel") {
    return make_task_builtin("cancel", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      if (current.task_value && current.task_value->cancelled) {
        current.task_value->cancelled->store(true, std::memory_order_relaxed);
      }
      return Value::nil();
    });
  }

  if (attribute.attribute == "send") {
    return make_channel_builtin("send", 1, [](Value& current, const std::vector<Value>& args) {
      return channel_send_value(current, args[0]);
    });
  }

  if (attribute.attribute == "recv") {
    return make_channel_builtin("recv", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return channel_recv_value(current);
    });
  }

  if (attribute.attribute == "close") {
    return make_channel_builtin("close", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return channel_close_value(current);
    });
  }

  if (attribute.attribute == "stats") {
    return make_channel_builtin("stats", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return channel_stats_value(current);
    });
  }

  if (attribute.attribute == "anext" || attribute.attribute == "next") {
    return make_channel_builtin(attribute.attribute, 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return stream_next_value(current);
    });
  }

  if (attribute.attribute == "has_next") {
    return make_channel_builtin("has_next", 0, [](Value& current, const std::vector<Value>& args) {
      (void)args;
      return stream_has_next_value(current);
    });
  }

  if (!target_is_variable) {
    throw EvalException(attribute.attribute + "() target must be a variable");
  }

  if (attribute.attribute == "append") {
    return make_list_value_builtin("append", 1, [](Value& current, const std::vector<Value>& args) {
      current.list_value.push_back(args[0]);
      invalidate_list_cache(current);
      return Value::nil();
    });
  }

  if (attribute.attribute == "pop") {
    return Value::builtin("pop", [env, target_name](const std::vector<Value>& args) -> Value {
      if (args.size() > 1) {
        throw EvalException("pop() expects at most one argument");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind != Value::Kind::List) {
        throw EvalException("pop() only supported on list variables");
      }
      if (current->list_value.empty()) {
        throw EvalException("pop from empty list");
      }

      long long index = static_cast<long long>(current->list_value.size()) - 1;
      if (!args.empty()) {
        index = value_to_int(args[0]);
      }
      if (index < 0) {
        index += static_cast<long long>(current->list_value.size());
      }
      if (index < 0 || static_cast<std::size_t>(index) >= current->list_value.size()) {
        throw EvalException("pop index out of range");
      }
      const std::size_t position = static_cast<std::size_t>(index);
      const auto value = current->list_value[position];
      current->list_value.erase(current->list_value.begin() + static_cast<long long>(position));
      invalidate_list_cache(*current);
      return value;
    });
  }

  if (attribute.attribute == "insert") {
    return Value::builtin("insert", [env, target_name](const std::vector<Value>& args) -> Value {
      if (args.size() != 2) {
        throw EvalException("insert() expects exactly two arguments");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind != Value::Kind::List) {
        throw EvalException("insert() only supported on list variables");
      }
      auto index = value_to_int(args[0]);
      if (index < 0) {
        index += static_cast<long long>(current->list_value.size());
      }
      if (index < 0) {
        index = 0;
      }
      if (static_cast<std::size_t>(index) > current->list_value.size()) {
        index = static_cast<long long>(current->list_value.size());
      }
      current->list_value.insert(current->list_value.begin() + index, args[1]);
      invalidate_list_cache(*current);
      return Value::nil();
    });
  }

  if (attribute.attribute == "remove") {
    return Value::builtin("remove", [env, target_name](const std::vector<Value>& args) -> Value {
      if (args.size() != 1) {
        throw EvalException("remove() expects exactly one argument");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind != Value::Kind::List) {
        throw EvalException("remove() only supported on list variables");
      }
      const auto& needle = args[0];
      auto it = current->list_value.begin();
      while (it != current->list_value.end()) {
        if (it->equals(needle)) {
          current->list_value.erase(it);
          invalidate_list_cache(*current);
          return Value::nil();
        }
        ++it;
      }
      throw EvalException("remove() could not find value");
    });
  }

  if (attribute.attribute == "reduce_sum") {
    return Value::builtin("reduce_sum", [env, target_name](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        throw EvalException("reduce_sum() expects no arguments");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind == Value::Kind::List) {
        return list_reduce_sum_with_plan(*current);
      }
      if (current->kind == Value::Kind::Matrix) {
        return matrix_reduce_sum_with_plan(*current);
      }
      throw EvalException("reduce_sum() supports list or matrix receivers");
    });
  }

  if (attribute.attribute == "map_add") {
    return Value::builtin("map_add", [env, target_name](const std::vector<Value>& args) -> Value {
      if (args.size() != 1) {
        throw EvalException("map_add() expects exactly one numeric argument");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind != Value::Kind::List) {
        throw EvalException("map_add() currently supports list receivers");
      }
      return list_map_add_with_plan(*current, args[0]);
    });
  }

  if (attribute.attribute == "plan_id") {
    return Value::builtin("plan_id", [env, target_name](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        throw EvalException("plan_id() expects no arguments");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind == Value::Kind::List) {
        return list_plan_id_value(*current);
      }
      if (current->kind == Value::Kind::Matrix) {
        return matrix_plan_id_value(*current);
      }
      throw EvalException("plan_id() supports list or matrix receivers");
    });
  }

  if (attribute.attribute == "cache_stats") {
    return Value::builtin("cache_stats", [env, target_name](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        throw EvalException("cache_stats() expects no arguments");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind == Value::Kind::List) {
        return list_cache_stats_value(*current);
      }
      if (current->kind == Value::Kind::Matrix) {
        return matrix_cache_stats_value(*current);
      }
      throw EvalException("cache_stats() supports list or matrix receivers");
    });
  }

  if (attribute.attribute == "cache_bytes") {
    return Value::builtin("cache_bytes", [env, target_name](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        throw EvalException("cache_bytes() expects no arguments");
      }
      auto* current = env->get_ptr(target_name);
      if (!current) {
        throw EvalException("undefined variable: " + target_name);
      }
      if (current->kind == Value::Kind::List) {
        return list_cache_bytes_value(*current);
      }
      if (current->kind == Value::Kind::Matrix) {
        return matrix_cache_bytes_value(*current);
      }
      throw EvalException("cache_bytes() supports list or matrix receivers");
    });
  }

  if (attribute.attribute == "pipeline_stats") {
    return Value::builtin("pipeline_stats", [env, target_name](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        throw EvalException("pipeline_stats() expects no arguments");
      }
      return pipeline_stats_value(env, target_name);
    });
  }

  if (attribute.attribute == "pipeline_plan_id") {
    return Value::builtin("pipeline_plan_id", [env, target_name](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        throw EvalException("pipeline_plan_id() expects no arguments");
      }
      return pipeline_plan_id_value(env, target_name);
    });
  }

  throw EvalException("unsupported attribute: " + attribute.attribute);
}

}  // namespace spark
