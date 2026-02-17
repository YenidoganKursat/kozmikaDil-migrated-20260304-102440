#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"
#include "phase9_runtime.h"

namespace spark {

namespace {

Value invoke_function_sync(const std::shared_ptr<Value::Function>& fn, const std::vector<Value>& args) {
  if (!fn || !fn->body) {
    throw EvalException("invalid function value");
  }
  if (fn->params.size() != args.size()) {
    throw EvalException("function argument count mismatch");
  }

  auto parent_env = fn->closure_frozen;
  if (!parent_env && fn->closure) {
    parent_env = std::make_shared<Environment>(nullptr, true);
    for (const auto& name : fn->closure->keys()) {
      parent_env->define(name, fn->closure->get(name));
    }
  }

  auto local_env = std::make_shared<Environment>(parent_env);
  for (std::size_t i = 0; i < fn->params.size(); ++i) {
    local_env->define(fn->params[i], args[i]);
  }

  Interpreter interpreter;
  Value result = Value::nil();
  try {
    for (const auto& stmt : *fn->body) {
      result = interpreter.execute(*stmt, local_env);
    }
  } catch (const Interpreter::ReturnSignal& signal) {
    return signal.value;
  }
  return result;
}

}  // namespace

Value invoke_callable_sync(const Value& callee, const std::vector<Value>& args) {
  if (callee.kind == Value::Kind::Builtin && callee.builtin_value) {
    return callee.builtin_value->impl(args);
  }
  if (callee.kind == Value::Kind::Function) {
    return invoke_function_sync(callee.function_value, args);
  }
  throw EvalException("attempted to call non-callable value");
}

Value await_task_value(const Value& task, const std::optional<long long>& timeout_ms) {
  if (task.kind != Value::Kind::Task || !task.task_value) {
    return task;
  }
  const auto& future = task.task_value->future;
  if (!future.valid()) {
    throw EvalException("await on invalid task");
  }
  if (timeout_ms.has_value()) {
    if (*timeout_ms < 0) {
      throw EvalException("await timeout must be non-negative");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*timeout_ms);
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      if (std::chrono::steady_clock::now() >= deadline) {
        throw EvalException("await timeout");
      }
      bool assisted = false;
      if (task.task_value->has_scheduler_queue_hint) {
        assisted = phase9::scheduler_assist_one_from_queue(task.task_value->scheduler_queue_hint);
      }
      if (!assisted) {
        assisted = phase9::scheduler_assist_one();
      }
      if (!assisted) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          throw EvalException("await timeout");
        }
        const auto status = future.wait_for(std::min<std::chrono::milliseconds>(
            std::chrono::milliseconds(1), remaining));
        if (status != std::future_status::ready &&
            std::chrono::steady_clock::now() >= deadline) {
          throw EvalException("await timeout");
        }
      }
    }
  } else {
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      bool assisted = false;
      if (task.task_value->has_scheduler_queue_hint) {
        assisted = phase9::scheduler_assist_one_from_queue(task.task_value->scheduler_queue_hint);
      }
      if (!assisted) {
        assisted = phase9::scheduler_assist_one();
      }
      if (!assisted) {
        future.wait();
        break;
      }
    }
  }
  try {
    return future.get();
  } catch (const EvalException&) {
    throw;
  } catch (const std::exception& err) {
    throw EvalException(std::string("task execution failed: ") + err.what());
  }
}

Value spawn_task_value(const Value& callee, const std::vector<Value>& args,
                       const std::shared_ptr<std::atomic<bool>>& cancel_token) {
  if (callee.kind != Value::Kind::Function && callee.kind != Value::Kind::Builtin) {
    throw EvalException("spawn() expects callable first argument");
  }

  auto token = cancel_token;
  if (!token) {
    token = std::make_shared<std::atomic<bool>>(false);
  }

  std::size_t queue_hint = 0;
  auto future = phase9::scheduler_submit([callee, args, token]() -> Value {
    if (token->load(std::memory_order_relaxed)) {
      return Value::nil();
    }
    auto result = invoke_callable_sync(callee, args);
    if (result.kind == Value::Kind::Task) {
      return await_task_value(result);
    }
    return result;
  }, &queue_hint);

  auto task = std::make_shared<TaskHandle>();
  task->future = std::move(future);
  task->cancelled = std::move(token);
  task->scheduler_queue_hint = queue_hint;
  task->has_scheduler_queue_hint = true;
  return Value::task_value_of(std::move(task));
}

Value make_task_group_value(const std::optional<long long>& timeout_ms) {
  auto group = std::make_shared<TaskGroupHandle>();
  group->cancelled = std::make_shared<std::atomic<bool>>(false);
  if (timeout_ms.has_value()) {
    group->timeout_ms = *timeout_ms;
  }
  return Value::task_group_value_of(std::move(group));
}

Value task_group_spawn_value(Value& group, const Value& callee, const std::vector<Value>& args) {
  if (group.kind != Value::Kind::TaskGroup || !group.task_group_value) {
    throw EvalException("task_group.spawn() receiver must be task_group");
  }
  auto task = spawn_task_value(callee, args, group.task_group_value->cancelled);
  {
    std::lock_guard<std::mutex> guard(group.task_group_value->mutex);
    group.task_group_value->tasks.push_back(task.task_value);
  }
  return task;
}

Value task_group_join_all_value(Value& group) {
  if (group.kind != Value::Kind::TaskGroup || !group.task_group_value) {
    throw EvalException("task_group.join_all() receiver must be task_group");
  }

  std::vector<std::shared_ptr<TaskHandle>> tasks;
  {
    std::lock_guard<std::mutex> guard(group.task_group_value->mutex);
    tasks = group.task_group_value->tasks;
  }

  std::vector<Value> results;
  results.reserve(tasks.size());
  const auto timeout = group.task_group_value->timeout_ms >= 0
                           ? std::optional<long long>(group.task_group_value->timeout_ms)
                           : std::nullopt;
  for (const auto& task : tasks) {
    results.push_back(await_task_value(Value::task_value_of(task), timeout));
  }
  return Value::list_value_of(std::move(results));
}

Value task_group_cancel_all_value(Value& group) {
  if (group.kind != Value::Kind::TaskGroup || !group.task_group_value) {
    throw EvalException("task_group.cancel_all() receiver must be task_group");
  }
  if (group.task_group_value->cancelled) {
    group.task_group_value->cancelled->store(true, std::memory_order_relaxed);
  }
  return Value::nil();
}

Value scheduler_stats_value() {
  const auto stats = phase9::scheduler_stats_snapshot();
  std::vector<Value> out;
  out.reserve(4);
  out.push_back(Value::int_value_of(static_cast<long long>(stats.threads)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.spawned)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.executed)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.steals)));
  return Value::list_value_of(std::move(out));
}

}  // namespace spark
