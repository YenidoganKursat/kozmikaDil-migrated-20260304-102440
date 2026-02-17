#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"
#include "phase9_runtime.h"

namespace spark {
namespace {

std::size_t default_chunk_size(std::size_t total) {
  if (const auto* value = std::getenv("SPARK_PHASE9_CHUNK")) {
    const auto parsed = std::strtoull(value, nullptr, 10);
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  }

  if (total <= 64) {
    return total;
  }
  if (total <= 4096) {
    return 64;
  }
  return 512;
}

long long to_i64_checked(const Value& value, const std::string& where) {
  if (value.kind == Value::Kind::Int) {
    return value.int_value;
  }
  throw EvalException(where + " expects integer value");
}

struct BatchState {
  explicit BatchState(std::size_t tasks) : remaining(tasks) {}

  std::atomic<std::size_t> remaining;
  std::mutex mutex;
  std::condition_variable cv;
  std::exception_ptr first_error = nullptr;
};

void record_batch_error(const std::shared_ptr<BatchState>& state, std::exception_ptr error) {
  if (!error) {
    return;
  }
  std::lock_guard<std::mutex> guard(state->mutex);
  if (!state->first_error) {
    state->first_error = error;
  }
}

void complete_batch_task(const std::shared_ptr<BatchState>& state) {
  if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->cv.notify_one();
  }
}

void wait_batch(const std::shared_ptr<BatchState>& state) {
  std::unique_lock<std::mutex> lock(state->mutex);
  state->cv.wait(lock, [&state]() { return state->remaining.load(std::memory_order_acquire) == 0; });
  if (state->first_error) {
    std::rethrow_exception(state->first_error);
  }
}

}  // namespace

Value parallel_for_value(const Value& start, const Value& stop, const Value& fn,
                         const std::vector<Value>& extra_args) {
  if (fn.kind != Value::Kind::Function && fn.kind != Value::Kind::Builtin) {
    throw EvalException("parallel_for() expects callable function argument");
  }

  const auto begin = to_i64_checked(start, "parallel_for()");
  const auto end = to_i64_checked(stop, "parallel_for()");
  if (end <= begin) {
    return Value::nil();
  }

  const auto total = static_cast<std::size_t>(end - begin);
  const auto chunk = std::max<std::size_t>(1, default_chunk_size(total));
  const auto task_count = (total + chunk - 1) / chunk;
  if (task_count == 1) {
    std::vector<Value> args(1 + extra_args.size(), Value::nil());
    for (std::size_t k = 0; k < extra_args.size(); ++k) {
      args[k + 1] = extra_args[k];
    }
    for (long long i = begin; i < end; ++i) {
      args[0] = Value::int_value_of(i);
      (void)invoke_callable_sync(fn, args);
    }
    return Value::nil();
  }

  auto batch = std::make_shared<BatchState>(task_count);
  for (std::size_t task_index = 0, offset = 0; task_index < task_count; ++task_index, offset += chunk) {
    const auto chunk_start = begin + static_cast<long long>(offset);
    const auto chunk_stop = begin + static_cast<long long>(std::min(total, offset + chunk));
    phase9::scheduler_submit_fire_and_forget([fn, extra_args, chunk_start, chunk_stop, batch]() {
      try {
        std::vector<Value> args(1 + extra_args.size(), Value::nil());
        for (std::size_t k = 0; k < extra_args.size(); ++k) {
          args[k + 1] = extra_args[k];
        }
        for (long long i = chunk_start; i < chunk_stop; ++i) {
          args[0] = Value::int_value_of(i);
          (void)invoke_callable_sync(fn, args);
        }
      } catch (...) {
        record_batch_error(batch, std::current_exception());
      }
      complete_batch_task(batch);
    });
  }
  wait_batch(batch);
  return Value::nil();
}

Value par_map_value(const Value& list, const Value& fn) {
  if (list.kind != Value::Kind::List) {
    throw EvalException("par_map() expects list as first argument");
  }
  if (fn.kind != Value::Kind::Function && fn.kind != Value::Kind::Builtin) {
    throw EvalException("par_map() expects callable function argument");
  }
  const auto total = list.list_value.size();
  if (total == 0) {
    return Value::list_value_of({});
  }

  std::vector<Value> out(total);
  const auto chunk = std::max<std::size_t>(1, default_chunk_size(total));
  const auto* input = &list.list_value;
  const auto task_count = (total + chunk - 1) / chunk;
  if (task_count == 1) {
    std::vector<Value> one_arg(1, Value::nil());
    for (std::size_t i = 0; i < total; ++i) {
      one_arg[0] = (*input)[i];
      out[i] = invoke_callable_sync(fn, one_arg);
    }
    return Value::list_value_of(std::move(out));
  }

  auto batch = std::make_shared<BatchState>(task_count);
  for (std::size_t task_index = 0, offset = 0; task_index < task_count; ++task_index, offset += chunk) {
    const auto begin = offset;
    const auto finish = std::min(total, offset + chunk);
    phase9::scheduler_submit_fire_and_forget([fn, input, &out, begin, finish, batch]() {
      try {
        std::vector<Value> one_arg(1, Value::nil());
        for (std::size_t i = begin; i < finish; ++i) {
          one_arg[0] = (*input)[i];
          out[i] = invoke_callable_sync(fn, one_arg);
        }
      } catch (...) {
        record_batch_error(batch, std::current_exception());
      }
      complete_batch_task(batch);
    });
  }
  wait_batch(batch);
  return Value::list_value_of(std::move(out));
}

Value par_reduce_value(const Value& list, const Value& init, const Value& fn) {
  if (list.kind != Value::Kind::List) {
    throw EvalException("par_reduce() expects list as first argument");
  }
  if (fn.kind != Value::Kind::Function && fn.kind != Value::Kind::Builtin) {
    throw EvalException("par_reduce() expects callable reducer argument");
  }
  if (list.list_value.empty()) {
    return init;
  }

  const auto total = list.list_value.size();
  const auto chunk = std::max<std::size_t>(1, default_chunk_size(total));
  const auto* input = &list.list_value;
  const auto task_count = (total + chunk - 1) / chunk;
  if (task_count == 1) {
    std::vector<Value> reduce_args(2, Value::nil());
    Value acc = init;
    for (const auto& item : *input) {
      reduce_args[0] = acc;
      reduce_args[1] = item;
      acc = invoke_callable_sync(fn, reduce_args);
    }
    return acc;
  }

  std::vector<Value> partials(task_count);
  auto batch = std::make_shared<BatchState>(task_count);
  for (std::size_t task_index = 0, offset = 0; task_index < task_count; ++task_index, offset += chunk) {
    const auto begin = offset;
    const auto finish = std::min(total, offset + chunk);
    phase9::scheduler_submit_fire_and_forget([fn, input, &partials, task_index, begin, finish, batch]() {
      try {
        std::vector<Value> reduce_args(2, Value::nil());
        Value local = (*input)[begin];
        for (std::size_t i = begin + 1; i < finish; ++i) {
          reduce_args[0] = local;
          reduce_args[1] = (*input)[i];
          local = invoke_callable_sync(fn, reduce_args);
        }
        partials[task_index] = std::move(local);
      } catch (...) {
        record_batch_error(batch, std::current_exception());
      }
      complete_batch_task(batch);
    });
  }
  wait_batch(batch);

  Value acc = init;
  std::vector<Value> reduce_args(2, Value::nil());
  for (auto& partial : partials) {
    reduce_args[0] = acc;
    reduce_args[1] = partial;
    acc = invoke_callable_sync(fn, reduce_args);
  }
  return acc;
}

}  // namespace spark
