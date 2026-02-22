#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "../internal_helpers.h"

namespace spark {

namespace {

bool assign_inplace_numeric_enabled() {
  // Single runtime policy: keep in-place numeric updates enabled.
  return true;
}

bool assign_direct_numeric_binary_enabled() {
  static const bool enabled = env_flag_enabled("SPARK_ASSIGN_DIRECT_NUMERIC_BINARY", true);
  return enabled;
}

bool recurrence_quickening_enabled() {
  static const bool enabled = env_flag_enabled("SPARK_ASSIGN_RECURRENCE_CACHE", true);
  return enabled;
}

bool recurrence_arith_quickening_enabled() {
  static const bool enabled = env_flag_enabled("SPARK_ASSIGN_RECURRENCE_ARITH", true);
  return enabled;
}

bool should_use_recurrence_quickening(const Value& current, BinaryOp op) {
  if (!is_numeric_kind(current)) {
    return false;
  }
  if (op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow) {
    return true;
  }
  if (op != BinaryOp::Add && op != BinaryOp::Sub && op != BinaryOp::Mul) {
    return false;
  }
  if (!recurrence_arith_quickening_enabled()) {
    return false;
  }

  if (current.kind == Value::Kind::Int || current.kind == Value::Kind::Double) {
    return true;
  }
  if (current.kind == Value::Kind::Numeric && current.numeric_value.has_value()) {
    return true;
  }
  return false;
}

bool is_numeric_arithmetic_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

bool assign_direct_numeric_kind_supported(const Value& current) {
  if (current.kind == Value::Kind::Int || current.kind == Value::Kind::Double) {
    return true;
  }
  if (current.kind != Value::Kind::Numeric || !current.numeric_value) {
    return false;
  }
  // Keep direct-path scoped to low/medium float families.
  // Exclude integer-numeric widened lanes (I64/I128/...) to preserve existing
  // list/int semantics in phase tests.
  const auto kind = current.numeric_value->kind;
  return numeric_kind_is_float(kind);
}

long long assign_fast_bench_tick_i64() {
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
  return static_cast<long long>(ns);
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
    return static_cast<long long>(ticks);
  }
  return static_cast<long long>(static_cast<long double>(ticks) * tick_to_ns);
#elif defined(CLOCK_MONOTONIC_RAW)
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + static_cast<long long>(ts.tv_nsec);
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

long long assign_fast_bench_tick_raw_i64() {
#if defined(__APPLE__) && defined(__aarch64__)
  std::uint64_t ticks = 0;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return static_cast<long long>(ticks);
#elif defined(__APPLE__)
  return static_cast<long long>(mach_absolute_time());
#elif defined(CLOCK_MONOTONIC_RAW)
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + static_cast<long long>(ts.tv_nsec);
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

long long assign_fast_bench_tick_scale_num_i64() {
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

long long assign_fast_bench_tick_scale_den_i64() {
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

bool assign_try_eval_fast_noarg_builtin(const Value::Builtin& builtin, Value& out) {
  if (builtin.name == "bench_tick") {
    out = Value::int_value_of(assign_fast_bench_tick_i64());
    return true;
  }
  if (builtin.name == "bench_tick_raw") {
    out = Value::int_value_of(assign_fast_bench_tick_raw_i64());
    return true;
  }
  if (builtin.name == "bench_tick_scale_num") {
    out = Value::int_value_of(assign_fast_bench_tick_scale_num_i64());
    return true;
  }
  if (builtin.name == "bench_tick_scale_den") {
    out = Value::int_value_of(assign_fast_bench_tick_scale_den_i64());
    return true;
  }
  return false;
}

bool assign_direct_operands_compatible(const Value& current, const Value& left,
                                       const Value& right) {
  if (current.kind == Value::Kind::Int) {
    return left.kind == Value::Kind::Int && right.kind == Value::Kind::Int;
  }
  if (current.kind == Value::Kind::Double) {
    const auto scalar_like = [](const Value& v) {
      return v.kind == Value::Kind::Int || v.kind == Value::Kind::Double;
    };
    return scalar_like(left) && scalar_like(right);
  }
  if (current.kind == Value::Kind::Numeric && current.numeric_value) {
    return left.kind == Value::Kind::Numeric && right.kind == Value::Kind::Numeric &&
           left.numeric_value && right.numeric_value &&
           left.numeric_value->kind == current.numeric_value->kind &&
           right.numeric_value->kind == current.numeric_value->kind;
  }
  return false;
}

const VariableExpr* as_variable_expr_assign(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return static_cast<const VariableExpr*>(expr);
}

const CallExpr* as_call_expr_assign(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Call) {
    return nullptr;
  }
  return static_cast<const CallExpr*>(expr);
}

bool try_inplace_numeric_constructor_assign(const AssignStmt& assign, Interpreter& self,
                                            const std::shared_ptr<Environment>& env,
                                            Value& current) {
  if (!assign.value) {
    return false;
  }
  const auto* call = as_call_expr_assign(assign.value.get());
  if (!call || call->args.size() != 1 || !call->callee ||
      call->callee->kind != Expr::Kind::Variable) {
    return false;
  }
  if (current.kind != Value::Kind::Numeric || !current.numeric_value) {
    return false;
  }

  const auto& callee_var = static_cast<const VariableExpr&>(*call->callee);
  const auto* callee = env->get_ptr(callee_var.name);
  if (!callee || callee->kind != Value::Kind::Builtin || !callee->builtin_value) {
    return false;
  }

  Value::NumericKind target_kind = Value::NumericKind::F64;
  try {
    target_kind = numeric_kind_from_name(callee->builtin_value->name);
  } catch (const EvalException&) {
    return false;
  }

  if (current.numeric_value->kind != target_kind) {
    return false;
  }

  const auto source = self.evaluate(*call->args[0], env);
  if (!is_numeric_kind(source)) {
    throw EvalException(callee->builtin_value->name +
                        "() expects exactly one numeric argument");
  }

  if (cast_numeric_to_kind_inplace(target_kind, source, current)) {
    return true;
  }
  const auto converted = cast_numeric_to_kind(target_kind, source);
  if (!copy_numeric_value_inplace(current, converted)) {
    current = converted;
  }
  return true;
}

bool invariant_numeric_operand_stamp(const Value& value, std::uint64_t& out_stamp) {
  switch (value.kind) {
    case Value::Kind::Int: {
      out_stamp = static_cast<std::uint64_t>(value.int_value);
      return true;
    }
    case Value::Kind::Double: {
      std::uint64_t bits = 0;
      std::memcpy(&bits, &value.double_value, sizeof(bits));
      out_stamp = bits;
      return true;
    }
    case Value::Kind::Numeric:
      break;
    default:
      return false;
  }
  if (!value.numeric_value) {
    return false;
  }
  const auto& numeric = *value.numeric_value;
  std::uint64_t stamp = static_cast<std::uint64_t>(numeric.kind) * 0x9e3779b97f4a7c15ULL;
  if (numeric_kind_is_high_precision_float(numeric.kind)) {
    const auto cache_addr =
        reinterpret_cast<std::uintptr_t>(numeric.high_precision_cache.get());
    stamp ^= static_cast<std::uint64_t>(cache_addr >> 4U);
    stamp ^= numeric.revision * 0xbf58476d1ce4e5b9ULL;
    out_stamp = stamp;
    return true;
  }
  if (numeric.parsed_int_valid) {
    const auto v = static_cast<unsigned __int128>(numeric.parsed_int);
    const auto lo = static_cast<std::uint64_t>(v);
    const auto hi = static_cast<std::uint64_t>(v >> 64U);
    stamp ^= lo;
    stamp ^= (hi * 0x94d049bb133111ebULL);
    out_stamp = stamp;
    return true;
  }
  if (numeric.parsed_float_valid) {
    static_assert(sizeof(long double) <= 16, "unexpected long double size");
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::memcpy(&lo, &numeric.parsed_float, sizeof(std::uint64_t));
    if (sizeof(long double) > sizeof(std::uint64_t)) {
      std::memcpy(&hi, reinterpret_cast<const std::uint8_t*>(&numeric.parsed_float) + sizeof(std::uint64_t),
                  sizeof(long double) - sizeof(std::uint64_t));
    }
    stamp ^= lo;
    stamp ^= (hi * 0x94d049bb133111ebULL);
    out_stamp = stamp;
    return true;
  }
  out_stamp = stamp ^ numeric.revision;
  return true;
}

bool high_precision_operand_stamp_fast(const Value& value, std::uint64_t& out_stamp) {
  if (value.kind != Value::Kind::Numeric || !value.numeric_value) {
    return false;
  }
  const auto& numeric = *value.numeric_value;
  if (!numeric_kind_is_high_precision_float(numeric.kind)) {
    return false;
  }
  const auto cache_addr =
      reinterpret_cast<std::uintptr_t>(numeric.high_precision_cache.get());
  out_stamp = (static_cast<std::uint64_t>(numeric.kind) * 0x9e3779b97f4a7c15ULL) ^
              static_cast<std::uint64_t>(cache_addr >> 4U) ^
              (numeric.revision * 0xbf58476d1ce4e5b9ULL);
  return true;
}

bool numeric_values_equal_fast(const Value& lhs, const Value& rhs) {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  if (lhs.kind == Value::Kind::Int) {
    return lhs.int_value == rhs.int_value;
  }
  if (lhs.kind == Value::Kind::Double) {
    return lhs.double_value == rhs.double_value;
  }
  if (lhs.kind != Value::Kind::Numeric || !lhs.numeric_value || !rhs.numeric_value) {
    return false;
  }

  const auto& ln = *lhs.numeric_value;
  const auto& rn = *rhs.numeric_value;
  if (ln.kind != rn.kind) {
    return false;
  }

#if defined(SPARK_HAS_MPFR)
  if (numeric_kind_is_high_precision_float(ln.kind)) {
    if (ln.high_precision_cache && rn.high_precision_cache &&
        ln.high_precision_cache.get() == rn.high_precision_cache.get()) {
      return true;
    }
  }
#endif

  if (ln.parsed_int_valid && rn.parsed_int_valid) {
    return ln.parsed_int == rn.parsed_int;
  }
  if (ln.parsed_float_valid && rn.parsed_float_valid) {
    return ln.parsed_float == rn.parsed_float;
  }
  if (!ln.payload.empty() && !rn.payload.empty()) {
    return ln.payload == rn.payload;
  }
  return false;
}

const Value* try_resolve_assign_operand(const Expr& expr, Interpreter& self,
                                        const std::shared_ptr<Environment>& env,
                                        Value& temp) {
  if (expr.kind == Expr::Kind::Variable) {
    const auto& variable = static_cast<const VariableExpr&>(expr);
    struct VarRefCacheEntry {
      const Expr* expr = nullptr;
      std::uint64_t env_id = 0;
      const Value* value = nullptr;
    };
    constexpr std::size_t kVarRefCacheSize = 512;
    static thread_local std::array<VarRefCacheEntry, kVarRefCacheSize> cache{};
    const auto raw_hash = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(&expr)) ^
                          (static_cast<std::size_t>(env->stable_id) * 11400714819323198485ull);
    auto& slot = cache[raw_hash & (kVarRefCacheSize - 1)];
    if (slot.expr == &expr && slot.env_id == env->stable_id && slot.value != nullptr) {
      return slot.value;
    }
    if (const auto* ref = env->get_ptr(variable.name); ref != nullptr) {
      slot.expr = &expr;
      slot.env_id = env->stable_id;
      slot.value = ref;
      return ref;
    }
  }
  if (expr.kind == Expr::Kind::Number) {
    const auto& number = static_cast<const NumberExpr&>(expr);
    temp = number.is_int ? Value::int_value_of(static_cast<long long>(number.value))
                         : Value::double_value_of(number.value);
    return &temp;
  }
  temp = self.evaluate(expr, env);
  return &temp;
}

struct InvariantNumericAssignCacheEntry {
  const Expr* expr = nullptr;
  std::uint64_t env_id = 0;
  Value* target_ptr = nullptr;
  const Value* left_ptr = nullptr;
  const Value* right_ptr = nullptr;
  bool left_revision_valid = false;
  bool right_revision_valid = false;
  std::uint64_t left_revision = 0;
  std::uint64_t right_revision = 0;
  std::uint64_t left_stamp = 0;
  std::uint64_t right_stamp = 0;
  std::uint64_t result_stamp = 0;
  Value result = Value::nil();
};

struct RecurrenceNumericAssignCacheEntry {
  const Expr* expr = nullptr;
  std::uint64_t env_id = 0;
  Value* target_ptr = nullptr;
  BinaryOp op = BinaryOp::Add;
  bool target_on_left = true;
  bool rhs_is_variable = false;
  std::string rhs_name;
  Value rhs_literal = Value::nil();
  bool rhs_revision_valid = false;
  std::uint64_t rhs_revision = 0;
  std::uint64_t rhs_stamp = 0;
};

InvariantNumericAssignCacheEntry& invariant_numeric_assign_cache_slot(const Expr* expr,
                                                                      std::uint64_t env_id) {
  constexpr std::size_t kCacheSize = 1024;
  static thread_local std::array<InvariantNumericAssignCacheEntry, kCacheSize> cache{};
  const auto raw_hash = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(expr)) ^
                        (static_cast<std::size_t>(env_id) * 11400714819323198485ull);
  return cache[raw_hash & (kCacheSize - 1)];
}

RecurrenceNumericAssignCacheEntry& recurrence_numeric_assign_cache_slot(const Expr* expr,
                                                                        std::uint64_t env_id) {
  constexpr std::size_t kCacheSize = 1024;
  static thread_local std::array<RecurrenceNumericAssignCacheEntry, kCacheSize> cache{};
  const auto raw_hash = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(expr)) ^
                        (static_cast<std::size_t>(env_id) * 1469598103934665603ull);
  return cache[raw_hash & (kCacheSize - 1)];
}

bool try_copy_cached_numeric_result(const Value& cached, Value& target) {
  return copy_numeric_value_inplace(target, cached);
}

bool try_apply_recurrence_assign_cache_fast(const AssignStmt& assign,
                                            const std::shared_ptr<Environment>& env,
                                            Value* current) {
  if (!assign.value || !current) {
    return false;
  }
  auto& slot = recurrence_numeric_assign_cache_slot(assign.value.get(), env->stable_id);
  if (slot.expr != assign.value.get() || slot.env_id != env->stable_id ||
      slot.target_ptr != current) {
    return false;
  }

  const Value* rhs_ptr = nullptr;
  if (slot.rhs_is_variable) {
    rhs_ptr = env->get_ptr(slot.rhs_name);
  } else {
    rhs_ptr = &slot.rhs_literal;
  }
  if (!rhs_ptr || !is_numeric_kind(*rhs_ptr) || !is_numeric_kind(*current)) {
    return false;
  }

  if (slot.rhs_revision_valid) {
    if (rhs_ptr->kind != Value::Kind::Numeric || !rhs_ptr->numeric_value ||
        rhs_ptr->numeric_value->revision != slot.rhs_revision) {
      return false;
    }
  } else {
    std::uint64_t rhs_stamp = 0;
    if (!high_precision_operand_stamp_fast(*rhs_ptr, rhs_stamp) &&
        !invariant_numeric_operand_stamp(*rhs_ptr, rhs_stamp)) {
      return false;
    }
    if (rhs_stamp != slot.rhs_stamp) {
      return false;
    }
  }

  const Value* left = slot.target_on_left ? current : rhs_ptr;
  const Value* right = slot.target_on_left ? rhs_ptr : current;
  if (eval_numeric_binary_value_inplace(slot.op, *left, *right, *current)) {
    if (slot.rhs_revision_valid && rhs_ptr->kind == Value::Kind::Numeric &&
        rhs_ptr->numeric_value) {
      slot.rhs_revision = rhs_ptr->numeric_value->revision;
    }
    return true;
  }
  if (is_numeric_kind(*left) && is_numeric_kind(*right)) {
    *current = eval_numeric_binary_value(slot.op, *left, *right);
    if (slot.rhs_revision_valid && rhs_ptr->kind == Value::Kind::Numeric &&
        rhs_ptr->numeric_value) {
      slot.rhs_revision = rhs_ptr->numeric_value->revision;
    }
    return true;
  }
  return false;
}

void update_recurrence_assign_cache(const AssignStmt& assign,
                                    const std::shared_ptr<Environment>& env,
                                    Value* current, BinaryOp op, bool target_on_left,
                                    const VariableExpr* rhs_var,
                                    const Value& rhs_value) {
  auto& slot = recurrence_numeric_assign_cache_slot(assign.value.get(), env->stable_id);
  slot.expr = assign.value.get();
  slot.env_id = env->stable_id;
  slot.target_ptr = current;
  slot.op = op;
  slot.target_on_left = target_on_left;
  slot.rhs_is_variable = rhs_var != nullptr;
  slot.rhs_name = rhs_var ? rhs_var->name : std::string();
  if (!slot.rhs_is_variable) {
    slot.rhs_literal = rhs_value;
  } else {
    slot.rhs_literal = Value::nil();
  }
  slot.rhs_revision_valid = rhs_value.kind == Value::Kind::Numeric &&
                            rhs_value.numeric_value.has_value();
  slot.rhs_revision = slot.rhs_revision_valid ? rhs_value.numeric_value->revision : 0;
  slot.rhs_stamp = 0;
  if (!slot.rhs_revision_valid) {
    invariant_numeric_operand_stamp(rhs_value, slot.rhs_stamp);
  }
}

bool try_apply_invariant_assign_cache_fast(const AssignStmt& assign,
                                           const std::shared_ptr<Environment>& env,
                                           Value* current) {
  if (!assign.value || !current) {
    return false;
  }
  auto& slot = invariant_numeric_assign_cache_slot(assign.value.get(), env->stable_id);
  if (slot.expr != assign.value.get() || slot.env_id != env->stable_id ||
      slot.target_ptr != current || slot.left_ptr == nullptr || slot.right_ptr == nullptr) {
    return false;
  }

  std::uint64_t left_stamp = 0;
  std::uint64_t right_stamp = 0;
  if (slot.left_revision_valid) {
    if (slot.left_ptr->kind != Value::Kind::Numeric || !slot.left_ptr->numeric_value ||
        slot.left_ptr->numeric_value->revision != slot.left_revision) {
      return false;
    }
    left_stamp = slot.left_stamp;
  } else {
    if (!high_precision_operand_stamp_fast(*slot.left_ptr, left_stamp) &&
        !invariant_numeric_operand_stamp(*slot.left_ptr, left_stamp)) {
      return false;
    }
  }
  if (slot.right_revision_valid) {
    if (slot.right_ptr->kind != Value::Kind::Numeric || !slot.right_ptr->numeric_value ||
        slot.right_ptr->numeric_value->revision != slot.right_revision) {
      return false;
    }
    right_stamp = slot.right_stamp;
  } else {
    if (!high_precision_operand_stamp_fast(*slot.right_ptr, right_stamp) &&
        !invariant_numeric_operand_stamp(*slot.right_ptr, right_stamp)) {
      return false;
    }
  }
  if (left_stamp != slot.left_stamp || right_stamp != slot.right_stamp) {
    return false;
  }

  if (numeric_values_equal_fast(*current, slot.result)) {
    return true;
  }

  std::uint64_t current_stamp = 0;
  if ((high_precision_operand_stamp_fast(*current, current_stamp) ||
       invariant_numeric_operand_stamp(*current, current_stamp)) &&
      current_stamp == slot.result_stamp) {
    return true;
  }

  if (try_copy_cached_numeric_result(slot.result, *current)) {
    return true;
  }
  *current = slot.result;
  return true;
}

}  // namespace

Value execute_case_assign(const AssignStmt& assign, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  if (assign.target->kind == Expr::Kind::Variable) {
    const auto& variable = static_cast<const VariableExpr&>(*assign.target);
    const bool inplace_numeric_assign = assign_inplace_numeric_enabled();
    if (auto* current = env->get_ptr(variable.name); current != nullptr) {
      // Generic fast path: assignment from no-arg builtin call avoids full
      // expression dispatch and argument vector construction.
      if (assign.value) {
        if (const auto* call = as_call_expr_assign(assign.value.get());
            call && call->args.empty() && call->callee &&
            call->callee->kind == Expr::Kind::Variable) {
          const auto& callee_var = static_cast<const VariableExpr&>(*call->callee);
          if (const auto* callee = env->get_ptr(callee_var.name);
              callee && callee->kind == Value::Kind::Builtin && callee->builtin_value) {
            Value out = Value::nil();
            if (!assign_try_eval_fast_noarg_builtin(*callee->builtin_value, out)) {
              static const std::vector<Value> kNoArgs;
              out = callee->builtin_value->impl(kNoArgs);
            }
            if (!copy_numeric_value_inplace(*current, out)) {
              *current = std::move(out);
            }
            return Value::nil();
          }
        }

        // Generic in-place numeric constructor path:
        // x = fN(expr) where x already has kind fN.
        if (try_inplace_numeric_constructor_assign(assign, self, env, *current)) {
          return Value::nil();
        }
      }

      if (inplace_numeric_assign && assign.value && assign.value->kind == Expr::Kind::Binary) {
        const auto& binary = static_cast<const BinaryExpr&>(*assign.value);
        if (assign_direct_numeric_binary_enabled() &&
            is_numeric_arithmetic_op(binary.op) &&
            assign_direct_numeric_kind_supported(*current)) {
          const auto* left_var = as_variable_expr_assign(binary.left.get());
          const auto* right_var = as_variable_expr_assign(binary.right.get());
          if (left_var && right_var) {
            const Value* left = env->get_ptr(left_var->name);
            const Value* right = env->get_ptr(right_var->name);
            if (left && right && is_numeric_kind(*left) && is_numeric_kind(*right) &&
                assign_direct_operands_compatible(*current, *left, *right)) {
              if (current->kind == Value::Kind::Int &&
                  left->kind == Value::Kind::Int &&
                  right->kind == Value::Kind::Int) {
                switch (binary.op) {
                  case BinaryOp::Add:
                    current->int_value = left->int_value + right->int_value;
                    return Value::nil();
                  case BinaryOp::Sub:
                    current->int_value = left->int_value - right->int_value;
                    return Value::nil();
                  case BinaryOp::Mul:
                    current->int_value = left->int_value * right->int_value;
                    return Value::nil();
                  case BinaryOp::Div:
                    if (right->int_value == 0) {
                      throw EvalException("division by zero");
                    }
                    *current = Value::double_value_of(
                        static_cast<double>(left->int_value) /
                        static_cast<double>(right->int_value));
                    return Value::nil();
                  case BinaryOp::Mod:
                    if (right->int_value == 0) {
                      throw EvalException("modulo by zero");
                    }
                    current->int_value = left->int_value % right->int_value;
                    return Value::nil();
                  case BinaryOp::Pow:
                    break;
                  default:
                    break;
                }
              }
              if (current->kind == Value::Kind::Double &&
                  (left->kind == Value::Kind::Int || left->kind == Value::Kind::Double) &&
                  (right->kind == Value::Kind::Int || right->kind == Value::Kind::Double)) {
                const double lhs = (left->kind == Value::Kind::Int)
                                       ? static_cast<double>(left->int_value)
                                       : left->double_value;
                const double rhs = (right->kind == Value::Kind::Int)
                                       ? static_cast<double>(right->int_value)
                                       : right->double_value;
                switch (binary.op) {
                  case BinaryOp::Add:
                    current->double_value = lhs + rhs;
                    return Value::nil();
                  case BinaryOp::Sub:
                    current->double_value = lhs - rhs;
                    return Value::nil();
                  case BinaryOp::Mul:
                    current->double_value = lhs * rhs;
                    return Value::nil();
                  case BinaryOp::Div:
                    if (rhs == 0.0) {
                      throw EvalException("division by zero");
                    }
                    current->double_value = lhs / rhs;
                    return Value::nil();
                  case BinaryOp::Mod:
                    if (rhs == 0.0) {
                      throw EvalException("modulo by zero");
                    }
                    current->double_value = std::fmod(lhs, rhs);
                    return Value::nil();
                  case BinaryOp::Pow:
                    current->double_value = std::pow(lhs, rhs);
                    return Value::nil();
                  default:
                    break;
                }
              }
              if (eval_numeric_binary_value_inplace(binary.op, *left, *right, *current)) {
                return Value::nil();
              }
              *current = eval_numeric_binary_value(binary.op, *left, *right);
              return Value::nil();
            }
          }
        }

        const bool recurrence_allowed =
            should_use_recurrence_quickening(*current, binary.op) &&
            recurrence_quickening_enabled();

        // Recurrence quickening for generic loops:
        // x = x op y  /  x = y op x
        if (recurrence_allowed &&
            try_apply_recurrence_assign_cache_fast(assign, env, current)) {
          return Value::nil();
        }

        // Hot-path: if this exact assignment already has a validated invariant
        // cache entry (same env/target and same operand stamps), skip full
        // expression resolution and reuse cached result immediately.
        if (try_apply_invariant_assign_cache_fast(assign, env, current)) {
          return Value::nil();
        }

        if (is_numeric_arithmetic_op(binary.op)) {
          Value left_temp = Value::nil();
          Value right_temp = Value::nil();
          const Value* left = try_resolve_assign_operand(*binary.left, self, env, left_temp);
          const Value* right = try_resolve_assign_operand(*binary.right, self, env, right_temp);
          const auto* left_var = as_variable_expr_assign(binary.left.get());
          const auto* right_var = as_variable_expr_assign(binary.right.get());
          const bool recurrence_target_left =
              left_var && left_var->name == variable.name &&
              ((right_var != nullptr) || binary.right->kind == Expr::Kind::Number);
          const bool recurrence_target_right =
              right_var && right_var->name == variable.name &&
              ((left_var != nullptr) || binary.left->kind == Expr::Kind::Number);
          const Value* recurrence_rhs = recurrence_target_left ? right : left;
          const auto* recurrence_rhs_var =
              recurrence_target_left ? right_var : left_var;
          const bool recurrence_numeric =
              (recurrence_target_left || recurrence_target_right) &&
              recurrence_rhs && is_numeric_kind(*current) && is_numeric_kind(*recurrence_rhs);

          const bool invariant_numeric_operands =
              left && right && left != current && right != current &&
              is_numeric_kind(*left) && is_numeric_kind(*right) &&
              ((left_var != nullptr) || binary.left->kind == Expr::Kind::Number) &&
              ((right_var != nullptr) || binary.right->kind == Expr::Kind::Number);
          std::uint64_t left_stamp = 0;
          std::uint64_t right_stamp = 0;
          if (invariant_numeric_operands) {
            if (!invariant_numeric_operand_stamp(*left, left_stamp) ||
                !invariant_numeric_operand_stamp(*right, right_stamp)) {
              left_stamp = 0;
              right_stamp = 0;
            }
            auto& slot = invariant_numeric_assign_cache_slot(assign.value.get(), env->stable_id);
            if (slot.expr == assign.value.get() &&
                slot.env_id == env->stable_id &&
                slot.target_ptr == current &&
                slot.left_ptr == left &&
                slot.right_ptr == right &&
                slot.left_stamp == left_stamp &&
                slot.right_stamp == right_stamp) {
              std::uint64_t current_stamp = 0;
              if (invariant_numeric_operand_stamp(*current, current_stamp) &&
                  current_stamp == slot.result_stamp) {
                return Value::nil();
              }
              *current = slot.result;
              return Value::nil();
            }
          }

          if (left && right &&
              eval_numeric_binary_value_inplace(binary.op, *left, *right, *current)) {
            if (recurrence_numeric) {
              if (recurrence_allowed) {
                update_recurrence_assign_cache(assign, env, current, binary.op,
                                               recurrence_target_left, recurrence_rhs_var,
                                               *recurrence_rhs);
              }
            }
            if (invariant_numeric_operands) {
              auto& slot = invariant_numeric_assign_cache_slot(assign.value.get(), env->stable_id);
              slot.expr = assign.value.get();
              slot.env_id = env->stable_id;
              slot.target_ptr = current;
              slot.left_ptr = left;
              slot.right_ptr = right;
              slot.left_revision_valid =
                  left->kind == Value::Kind::Numeric && left->numeric_value.has_value();
              slot.right_revision_valid =
                  right->kind == Value::Kind::Numeric && right->numeric_value.has_value();
              slot.left_revision =
                  slot.left_revision_valid ? left->numeric_value->revision : 0;
              slot.right_revision =
                  slot.right_revision_valid ? right->numeric_value->revision : 0;
              slot.left_stamp = left_stamp;
              slot.right_stamp = right_stamp;
              slot.result = *current;
              invariant_numeric_operand_stamp(slot.result, slot.result_stamp);
            }
            return Value::nil();
          }

          if (left && right &&
              left->kind == Value::Kind::Numeric &&
              right->kind == Value::Kind::Numeric) {
            *current = eval_numeric_binary_value(binary.op, *left, *right);
            if (recurrence_numeric) {
              if (recurrence_allowed) {
                update_recurrence_assign_cache(assign, env, current, binary.op,
                                               recurrence_target_left, recurrence_rhs_var,
                                               *recurrence_rhs);
              }
            }
            if (invariant_numeric_operands) {
              auto& slot = invariant_numeric_assign_cache_slot(assign.value.get(), env->stable_id);
              slot.expr = assign.value.get();
              slot.env_id = env->stable_id;
              slot.target_ptr = current;
              slot.left_ptr = left;
              slot.right_ptr = right;
              slot.left_revision_valid =
                  left->kind == Value::Kind::Numeric && left->numeric_value.has_value();
              slot.right_revision_valid =
                  right->kind == Value::Kind::Numeric && right->numeric_value.has_value();
              slot.left_revision =
                  slot.left_revision_valid ? left->numeric_value->revision : 0;
              slot.right_revision =
                  slot.right_revision_valid ? right->numeric_value->revision : 0;
              slot.left_stamp = left_stamp;
              slot.right_stamp = right_stamp;
              slot.result = *current;
              invariant_numeric_operand_stamp(slot.result, slot.result_stamp);
            }
            return Value::nil();
          }

          if (left && right) {
            *current = self.eval_binary(binary.op, *left, *right);
            return Value::nil();
          }

          return Value::nil();
        }
      }
      auto evaluated = self.evaluate(*assign.value, env);
      if (!copy_numeric_value_inplace(*current, evaluated)) {
        *current = std::move(evaluated);
      }
      return Value::nil();
    }
    auto value = self.evaluate(*assign.value, env);
    env->define(variable.name, value);
    return Value::nil();
  }

  if (assign.target->kind == Expr::Kind::Index) {
    auto value = self.evaluate(*assign.value, env);
    const auto target = flatten_index_target(*assign.target);
    if (!target.variable) {
      throw EvalException("invalid assignment target");
    }
    auto current = env->get(target.variable->name);
    const ExprEvaluator evaluator = [&self](const Expr& expr, const std::shared_ptr<Environment>& local_env) {
      return self.evaluate(expr, local_env);
    };
    assign_indexed_expression(evaluator, current, target.indices, 0, env, value);
    if (!env->set(target.variable->name, current)) {
      throw EvalException("cannot assign to undefined variable: " + target.variable->name);
    }
    return Value::nil();
  }
  throw EvalException("invalid assignment target");
}

}  // namespace spark
