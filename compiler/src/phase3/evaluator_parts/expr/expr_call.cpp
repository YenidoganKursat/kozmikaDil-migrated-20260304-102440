#include <vector>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "../internal_helpers.h"

namespace spark {

namespace {

bool extract_numeric_literal_text(const Expr& expr, std::string& out_text, bool& out_is_int) {
  if (expr.kind == Expr::Kind::Number) {
    const auto& number = static_cast<const NumberExpr&>(expr);
    if (!number.raw_text.empty()) {
      out_text = number.raw_text;
      out_is_int = number.is_int;
      return true;
    }
    return false;
  }

  if (expr.kind == Expr::Kind::Unary) {
    const auto& unary = static_cast<const UnaryExpr&>(expr);
    if (unary.op == UnaryOp::Neg && unary.operand &&
        unary.operand->kind == Expr::Kind::Number) {
      const auto& number = static_cast<const NumberExpr&>(*unary.operand);
      if (!number.raw_text.empty()) {
        out_text = "-" + number.raw_text;
        out_is_int = number.is_int;
        return true;
      }
    }
  }

  return false;
}

const Value* try_lookup_builtin_callee(const CallExpr& call,
                                       const std::shared_ptr<Environment>& env) {
  if (!call.callee || call.callee->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  struct BuiltinLookupCacheEntry {
    const CallExpr* expr = nullptr;
    std::uint64_t env_id = 0;
    const Value* value = nullptr;
  };
  constexpr std::size_t kBuiltinLookupCacheSize = 1024;
  static thread_local std::array<BuiltinLookupCacheEntry, kBuiltinLookupCacheSize> cache{};

  const auto key_expr = &call;
  const auto key_env = env ? env->stable_id : 0;
  const auto raw_hash =
      static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key_expr)) ^
      (static_cast<std::size_t>(key_env) * 11400714819323198485ull);
  auto& slot = cache[raw_hash & (kBuiltinLookupCacheSize - 1)];
  if (slot.expr == key_expr && slot.env_id == key_env && slot.value != nullptr &&
      slot.value->kind == Value::Kind::Builtin && slot.value->builtin_value) {
    return slot.value;
  }

  const auto& variable = static_cast<const VariableExpr&>(*call.callee);
  const auto* value = env ? env->get_ptr(variable.name) : nullptr;
  if (!value || value->kind != Value::Kind::Builtin || !value->builtin_value) {
    return nullptr;
  }
  slot.expr = key_expr;
  slot.env_id = key_env;
  slot.value = value;
  return value;
}

Value fast_bench_tick_value() {
#if defined(__APPLE__) && defined(__aarch64__)
  std::uint64_t ticks = 0;
  static const std::uint64_t freq = []() {
    std::uint64_t out = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(out));
    return out == 0U ? 1U : out;
  }();
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  const auto ns =
      static_cast<std::uint64_t>((static_cast<unsigned __int128>(ticks) * 1000000000ULL) / freq);
  return Value::int_value_of(static_cast<long long>(ns));
#elif defined(__APPLE__)
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    if (info.denom == 0U) {
      info.numer = 1U;
      info.denom = 1U;
    }
    return info;
  }();
  static const bool one_to_one = (timebase.numer == 1U && timebase.denom == 1U);
  static const long double tick_to_ns =
      static_cast<long double>(timebase.numer) / static_cast<long double>(timebase.denom);
  const std::uint64_t ticks = mach_absolute_time();
  if (one_to_one) {
    return Value::int_value_of(static_cast<long long>(ticks));
  }
  const auto ns = static_cast<long long>(static_cast<long double>(ticks) * tick_to_ns);
  return Value::int_value_of(ns);
#elif defined(CLOCK_MONOTONIC_RAW)
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  const auto ns =
      static_cast<long long>(ts.tv_sec) * 1000000000LL + static_cast<long long>(ts.tv_nsec);
  return Value::int_value_of(ns);
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return Value::int_value_of(static_cast<long long>(ns));
#endif
}

Value fast_bench_tick_raw_value() {
#if defined(__APPLE__) && defined(__aarch64__)
  std::uint64_t ticks = 0;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return Value::int_value_of(static_cast<long long>(ticks));
#elif defined(__APPLE__)
  return Value::int_value_of(static_cast<long long>(mach_absolute_time()));
#elif defined(CLOCK_MONOTONIC_RAW)
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  const auto ns =
      static_cast<long long>(ts.tv_sec) * 1000000000LL + static_cast<long long>(ts.tv_nsec);
  return Value::int_value_of(ns);
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return Value::int_value_of(static_cast<long long>(ns));
#endif
}

long long fast_bench_tick_scale_num_value() {
#if defined(__APPLE__) && defined(__aarch64__)
  return 1000000000LL;
#elif defined(__APPLE__)
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    if (info.denom == 0U) {
      info.numer = 1U;
      info.denom = 1U;
    }
    return info;
  }();
  return static_cast<long long>(timebase.numer);
#else
  return 1LL;
#endif
}

long long fast_bench_tick_scale_den_value() {
#if defined(__APPLE__) && defined(__aarch64__)
  static const std::uint64_t freq = []() {
    std::uint64_t out = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(out));
    return out == 0U ? 1U : out;
  }();
  return static_cast<long long>(freq);
#elif defined(__APPLE__)
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    if (info.denom == 0U) {
      info.numer = 1U;
      info.denom = 1U;
    }
    return info;
  }();
  return static_cast<long long>(timebase.denom);
#else
  return 1LL;
#endif
}

bool is_numeric_constructor_name(const std::string& name, Value::NumericKind& out_kind) {
  try {
    out_kind = numeric_kind_from_name(name);
    return true;
  } catch (const EvalException&) {
    return false;
  }
}

}  // namespace

Value evaluate_case_call(const CallExpr& call, Interpreter& self,
                        const std::shared_ptr<Environment>& env) {
  // Ultra-hot no-arg builtin call fast-path. This avoids generic callable
  // resolution/allocation overhead in tight loops (e.g. bench_tick probes).
  if (call.args.empty()) {
    if (const auto* builtin = try_lookup_builtin_callee(call, env); builtin && builtin->builtin_value) {
      if (builtin->builtin_value->name == "bench_tick") {
        return fast_bench_tick_value();
      }
      if (builtin->builtin_value->name == "bench_tick_raw") {
        return fast_bench_tick_raw_value();
      }
      if (builtin->builtin_value->name == "bench_tick_scale_num") {
        return Value::int_value_of(fast_bench_tick_scale_num_value());
      }
      if (builtin->builtin_value->name == "bench_tick_scale_den") {
        return Value::int_value_of(fast_bench_tick_scale_den_value());
      }
      static const std::vector<Value> kNoArgs;
      return builtin->builtin_value->impl(kNoArgs);
    }
  }

  // One-arg numeric constructor fast-path without generic call dispatch.
  if (call.args.size() == 1) {
    if (const auto* builtin = try_lookup_builtin_callee(call, env); builtin && builtin->builtin_value) {
      Value::NumericKind target_kind = Value::NumericKind::F64;
      if (is_numeric_constructor_name(builtin->builtin_value->name, target_kind)) {
        std::string literal_text;
        bool literal_is_int = false;
        if (call.args[0] &&
            extract_numeric_literal_text(*call.args[0], literal_text, literal_is_int)) {
          const auto source_kind =
              literal_is_int ? Value::NumericKind::I512 : Value::NumericKind::F512;
          const auto source = Value::numeric_value_of(source_kind, literal_text);
          return cast_numeric_to_kind(target_kind, source);
        }
        const auto value = self.evaluate(*call.args[0], env);
        if (!is_numeric_kind(value)) {
          throw EvalException(builtin->builtin_value->name +
                              "() expects exactly one numeric argument");
        }
        return cast_numeric_to_kind(target_kind, value);
      }
    }
  }

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
      std::string literal_text;
      bool literal_is_int = false;
      if (call.args[0] &&
          extract_numeric_literal_text(*call.args[0], literal_text, literal_is_int)) {
        const auto source_kind =
            literal_is_int ? Value::NumericKind::I512 : Value::NumericKind::F512;
        const auto source = Value::numeric_value_of(source_kind, literal_text);
        return cast_numeric_to_kind(target_kind, source);
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
