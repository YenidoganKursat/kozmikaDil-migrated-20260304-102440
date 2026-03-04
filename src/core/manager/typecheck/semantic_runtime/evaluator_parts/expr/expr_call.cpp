#include <vector>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string_view>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "../internal_helpers.h"

namespace spark {

namespace {

using I128 = spark_i128;
using U128 = spark_u128;

I128 ec_i128_max() {
  return static_cast<I128>((~U128{0}) >> 1U);
}

I128 ec_i128_min() {
  return -ec_i128_max() - 1;
}

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

const Value* try_lookup_global_variable_slot(const Expr* expr,
                                             const std::shared_ptr<Environment>& env) {
  if (!expr || expr->kind != Expr::Kind::Variable || !env || env->parent) {
    return nullptr;
  }
  struct GlobalVarLookupCacheEntry {
    const Expr* expr = nullptr;
    std::uint64_t env_id = 0;
    std::uint64_t values_epoch = 0;
    const Value* value = nullptr;
  };
  constexpr std::size_t kGlobalVarLookupCacheSize = 2048;
  static thread_local std::array<GlobalVarLookupCacheEntry, kGlobalVarLookupCacheSize> cache{};

  const auto key_expr = expr;
  const auto key_env = env->stable_id;
  const auto key_epoch = env->values_epoch;
  const auto raw_hash =
      static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key_expr)) ^
      (static_cast<std::size_t>(key_env) * 11400714819323198485ull) ^
      (static_cast<std::size_t>(key_epoch) * 14029467366897019727ull);
  auto& slot = cache[raw_hash & (kGlobalVarLookupCacheSize - 1)];
  if (slot.expr == key_expr && slot.env_id == key_env && slot.values_epoch == key_epoch &&
      slot.value != nullptr) {
    return slot.value;
  }

  const auto& variable = static_cast<const VariableExpr&>(*expr);
  const auto* value = env->get_ptr(variable.name);
  if (!value) {
    return nullptr;
  }
  slot.expr = key_expr;
  slot.env_id = key_env;
  slot.values_epoch = key_epoch;
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

std::string i128_to_string_fast(spark_i128 value) {
  if (value == 0) {
    return "0";
  }
  bool negative = value < 0;
  U128 magnitude = 0;
  if (negative) {
    magnitude = static_cast<U128>(-(value + 1));
    magnitude += 1;
  } else {
    magnitude = static_cast<U128>(value);
  }

  char buffer[64];
  std::size_t pos = 0;
  while (magnitude > 0) {
    const auto digit = static_cast<unsigned>(magnitude % 10);
    buffer[pos++] = static_cast<char>('0' + digit);
    magnitude /= 10;
  }
  std::string out;
  out.reserve(pos + (negative ? 1 : 0));
  if (negative) {
    out.push_back('-');
  }
  for (std::size_t i = 0; i < pos; ++i) {
    out.push_back(buffer[pos - 1 - i]);
  }
  return out;
}

bool try_value_to_i128_fast(const Value& value, I128& out) {
  if (value.kind == Value::Kind::Int) {
    out = static_cast<I128>(value.int_value);
    return true;
  }
  if (value.kind == Value::Kind::Numeric && value.numeric_value &&
      value.numeric_value->parsed_int_valid &&
      numeric_kind_is_int(value.numeric_value->kind)) {
    out = static_cast<I128>(value.numeric_value->parsed_int);
    return true;
  }
  return false;
}

bool try_eval_fast_int_expr_for_string(const Expr* expr, const std::shared_ptr<Environment>& env, I128& out) {
  if (!expr) {
    return false;
  }
  if (expr->kind == Expr::Kind::Variable) {
    const auto& var = static_cast<const VariableExpr&>(*expr);
    const auto* slot = env ? env->get_ptr(var.name) : nullptr;
    if (!slot) {
      return false;
    }
    return try_value_to_i128_fast(*slot, out);
  }
  if (expr->kind == Expr::Kind::Number) {
    const auto& number = static_cast<const NumberExpr&>(*expr);
    if (!number.is_int || !std::isfinite(number.value)) {
      return false;
    }
    if (number.value < static_cast<double>(std::numeric_limits<long long>::min()) ||
        number.value > static_cast<double>(std::numeric_limits<long long>::max())) {
      return false;
    }
    out = static_cast<I128>(static_cast<long long>(number.value));
    return true;
  }
  if (expr->kind == Expr::Kind::Unary) {
    const auto& unary = static_cast<const UnaryExpr&>(*expr);
    if (unary.op != UnaryOp::Neg) {
      return false;
    }
    I128 inner = 0;
    if (!try_eval_fast_int_expr_for_string(unary.operand.get(), env, inner)) {
      return false;
    }
    if (inner == ec_i128_min()) {
      out = ec_i128_max();
      return true;
    }
    out = -inner;
    return true;
  }
  if (expr->kind != Expr::Kind::Binary) {
    return false;
  }

  const auto& binary = static_cast<const BinaryExpr&>(*expr);
  I128 lhs = 0;
  I128 rhs = 0;
  if (!try_eval_fast_int_expr_for_string(binary.left.get(), env, lhs) ||
      !try_eval_fast_int_expr_for_string(binary.right.get(), env, rhs)) {
    return false;
  }

  switch (binary.op) {
    case BinaryOp::Add:
      if (__builtin_add_overflow(lhs, rhs, &out)) {
        out = (lhs >= 0 && rhs >= 0) ? ec_i128_max() : ec_i128_min();
      }
      return true;
    case BinaryOp::Sub:
      if (__builtin_sub_overflow(lhs, rhs, &out)) {
        out = (lhs >= 0 && rhs < 0) ? ec_i128_max() : ec_i128_min();
      }
      return true;
    case BinaryOp::Mul:
      if (__builtin_mul_overflow(lhs, rhs, &out)) {
        const bool non_negative = (lhs == 0 || rhs == 0) || ((lhs > 0) == (rhs > 0));
        out = non_negative ? ec_i128_max() : ec_i128_min();
      }
      return true;
    case BinaryOp::Mod:
      if (rhs == 0) {
        return false;
      }
      out = lhs % rhs;
      return true;
    default:
      return false;
  }
}

struct FastIntExprInstr {
  enum class Op : std::uint8_t {
    Const = 0,
    Var,
    Neg,
    Add,
    Sub,
    Mul,
    Mod,
  };
  Op op = Op::Const;
  I128 constant = 0;
  const Expr* var_expr = nullptr;
};

struct FastIntExprPlan {
  std::vector<FastIntExprInstr> code;
};

bool build_fast_int_expr_plan(const Expr* expr, FastIntExprPlan& out) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case Expr::Kind::Variable: {
      FastIntExprInstr ins;
      ins.op = FastIntExprInstr::Op::Var;
      ins.var_expr = expr;
      out.code.push_back(ins);
      return true;
    }
    case Expr::Kind::Number: {
      const auto& number = static_cast<const NumberExpr&>(*expr);
      if (!number.is_int || !std::isfinite(number.value)) {
        return false;
      }
      if (number.value < static_cast<double>(std::numeric_limits<long long>::min()) ||
          number.value > static_cast<double>(std::numeric_limits<long long>::max())) {
        return false;
      }
      FastIntExprInstr ins;
      ins.op = FastIntExprInstr::Op::Const;
      ins.constant = static_cast<I128>(static_cast<long long>(number.value));
      out.code.push_back(ins);
      return true;
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(*expr);
      if (unary.op != UnaryOp::Neg || !build_fast_int_expr_plan(unary.operand.get(), out)) {
        return false;
      }
      FastIntExprInstr ins;
      ins.op = FastIntExprInstr::Op::Neg;
      out.code.push_back(ins);
      return true;
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(*expr);
      if (!build_fast_int_expr_plan(binary.left.get(), out) ||
          !build_fast_int_expr_plan(binary.right.get(), out)) {
        return false;
      }
      FastIntExprInstr ins;
      switch (binary.op) {
        case BinaryOp::Add:
          ins.op = FastIntExprInstr::Op::Add;
          break;
        case BinaryOp::Sub:
          ins.op = FastIntExprInstr::Op::Sub;
          break;
        case BinaryOp::Mul:
          ins.op = FastIntExprInstr::Op::Mul;
          break;
        case BinaryOp::Mod:
          ins.op = FastIntExprInstr::Op::Mod;
          break;
        default:
          return false;
      }
      out.code.push_back(ins);
      return true;
    }
    default:
      return false;
  }
}

const FastIntExprPlan* fast_int_expr_plan_cached(const Expr* expr) {
  if (!expr) {
    return nullptr;
  }
  struct FastIntExprPlanCacheEntry {
    const Expr* expr = nullptr;
    bool initialized = false;
    bool supported = false;
    FastIntExprPlan plan;
  };
  constexpr std::size_t kFastIntExprPlanCacheSize = 512;
  static thread_local std::array<FastIntExprPlanCacheEntry, kFastIntExprPlanCacheSize> cache{};

  const auto raw_hash = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(expr)) ^
                        0x9e3779b97f4a7c15ull;
  auto& slot = cache[raw_hash & (kFastIntExprPlanCacheSize - 1)];
  if (slot.initialized && slot.expr == expr) {
    return slot.supported ? &slot.plan : nullptr;
  }

  slot.expr = expr;
  slot.initialized = true;
  slot.supported = false;
  slot.plan = FastIntExprPlan{};
  FastIntExprPlan plan;
  if (build_fast_int_expr_plan(expr, plan)) {
    slot.supported = true;
    slot.plan = std::move(plan);
  }
  return slot.supported ? &slot.plan : nullptr;
}

bool eval_fast_int_expr_plan(const FastIntExprPlan& plan,
                             const std::shared_ptr<Environment>& env, I128& out) {
  if (plan.code.empty()) {
    return false;
  }
  std::vector<I128> stack;
  stack.reserve(plan.code.size());
  for (const auto& ins : plan.code) {
    switch (ins.op) {
      case FastIntExprInstr::Op::Const:
        stack.push_back(ins.constant);
        break;
      case FastIntExprInstr::Op::Var: {
        if (!ins.var_expr || ins.var_expr->kind != Expr::Kind::Variable) {
          return false;
        }
        const auto& var = static_cast<const VariableExpr&>(*ins.var_expr);
        const auto* slot = try_lookup_global_variable_slot(ins.var_expr, env);
        if (!slot) {
          slot = env ? env->get_ptr(var.name) : nullptr;
        }
        if (!slot) {
          return false;
        }
        I128 value = 0;
        if (!try_value_to_i128_fast(*slot, value)) {
          return false;
        }
        stack.push_back(value);
        break;
      }
      case FastIntExprInstr::Op::Neg: {
        if (stack.empty()) {
          return false;
        }
        auto value = stack.back();
        stack.pop_back();
        if (value == ec_i128_min()) {
          value = ec_i128_max();
        } else {
          value = -value;
        }
        stack.push_back(value);
        break;
      }
      case FastIntExprInstr::Op::Add:
      case FastIntExprInstr::Op::Sub:
      case FastIntExprInstr::Op::Mul:
      case FastIntExprInstr::Op::Mod: {
        if (stack.size() < 2) {
          return false;
        }
        const auto rhs = stack.back();
        stack.pop_back();
        const auto lhs = stack.back();
        stack.pop_back();
        I128 value = 0;
        if (ins.op == FastIntExprInstr::Op::Add) {
          if (__builtin_add_overflow(lhs, rhs, &value)) {
            value = (lhs >= 0 && rhs >= 0) ? ec_i128_max() : ec_i128_min();
          }
        } else if (ins.op == FastIntExprInstr::Op::Sub) {
          if (__builtin_sub_overflow(lhs, rhs, &value)) {
            value = (lhs >= 0 && rhs < 0) ? ec_i128_max() : ec_i128_min();
          }
        } else if (ins.op == FastIntExprInstr::Op::Mul) {
          if (__builtin_mul_overflow(lhs, rhs, &value)) {
            const bool non_negative = (lhs == 0 || rhs == 0) || ((lhs > 0) == (rhs > 0));
            value = non_negative ? ec_i128_max() : ec_i128_min();
          }
        } else {
          if (rhs == 0) {
            return false;
          }
          value = lhs % rhs;
        }
        stack.push_back(value);
        break;
      }
    }
  }
  if (stack.size() != 1) {
    return false;
  }
  out = stack.back();
  return true;
}

std::string numeric_int_to_string_fast(const Value::NumericValue& numeric) {
  if (!numeric.parsed_int_valid) {
    return {};
  }
  if (numeric.kind == Value::NumericKind::I8 || numeric.kind == Value::NumericKind::I16 ||
      numeric.kind == Value::NumericKind::I32 || numeric.kind == Value::NumericKind::I64) {
    const auto v = static_cast<long long>(numeric.parsed_int);
    char buffer[32];
    auto* begin = buffer;
    auto* end = buffer + sizeof(buffer);
    const auto conv = std::to_chars(begin, end, v);
    if (conv.ec == std::errc{}) {
      return std::string(begin, static_cast<std::size_t>(conv.ptr - begin));
    }
  }
  return i128_to_string_fast(numeric.parsed_int);
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

struct CallArgsScratch {
  bool in_use = false;
  std::vector<Value> args;
};

CallArgsScratch& call_args_scratch() {
  thread_local CallArgsScratch scratch;
  return scratch;
}

struct CallArgsScratchGuard {
  explicit CallArgsScratchGuard(CallArgsScratch& state)
      : scratch(state) {
    scratch.in_use = true;
  }
  ~CallArgsScratchGuard() {
    scratch.args.clear();
    scratch.in_use = false;
  }
  CallArgsScratch& scratch;
};

Value instantiate_class_value(const Value& class_value, const std::vector<Value>& args) {
  if (class_value.kind != Value::Kind::Class || !class_value.class_value) {
    throw EvalException("attempted to instantiate non-class value");
  }
  auto object_payload = std::make_shared<Value::ObjectValue>();
  object_payload->class_def = class_value.class_value;
  object_payload->fields = class_value.class_value->field_defaults;
  Value instance = Value::object_value_of(object_payload);

  Value init_method = Value::nil();
  auto init_it = class_value.class_value->methods.find("__init__");
  if (init_it == class_value.class_value->methods.end()) {
    init_it = class_value.class_value->methods.find("init");
  }
  if (init_it != class_value.class_value->methods.end()) {
    init_method = init_it->second;
  }
  if (init_method.kind != Value::Kind::Nil) {
    std::vector<Value> init_args;
    init_args.reserve(args.size() + 1);
    init_args.push_back(instance);
    for (const auto& arg : args) {
      init_args.push_back(arg);
    }
    if (init_method.kind == Value::Kind::Function && init_method.function_value &&
        init_method.function_value->is_async) {
      throw EvalException("async class init is unsupported");
    } else {
      (void)invoke_callable_sync(init_method, init_args);
    }
  }
  return instance;
}

std::size_t fast_utf8_codepoint_count(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  std::size_t count = 0;
  bool ascii_only = true;
  for (unsigned char ch : text) {
    if ((ch & 0x80u) != 0u) {
      ascii_only = false;
    }
    if ((ch & 0xC0u) != 0x80u) {
      ++count;
    }
  }
  if (ascii_only) {
    return text.size();
  }
  return count;
}

std::size_t fast_utf16_units(std::string_view text) {
  std::size_t units = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto c0 = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = 0xFFFDu;
    std::size_t advance = 1;
    if ((c0 & 0x80u) == 0u) {
      cp = c0;
      advance = 1;
    } else if ((c0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
      const auto c1 = static_cast<unsigned char>(text[i + 1]);
      cp = (static_cast<std::uint32_t>(c0 & 0x1Fu) << 6) |
           static_cast<std::uint32_t>(c1 & 0x3Fu);
      advance = 2;
    } else if ((c0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
      const auto c1 = static_cast<unsigned char>(text[i + 1]);
      const auto c2 = static_cast<unsigned char>(text[i + 2]);
      cp = (static_cast<std::uint32_t>(c0 & 0x0Fu) << 12) |
           (static_cast<std::uint32_t>(c1 & 0x3Fu) << 6) |
           static_cast<std::uint32_t>(c2 & 0x3Fu);
      advance = 3;
    } else if ((c0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
      const auto c1 = static_cast<unsigned char>(text[i + 1]);
      const auto c2 = static_cast<unsigned char>(text[i + 2]);
      const auto c3 = static_cast<unsigned char>(text[i + 3]);
      cp = (static_cast<std::uint32_t>(c0 & 0x07u) << 18) |
           (static_cast<std::uint32_t>(c1 & 0x3Fu) << 12) |
           (static_cast<std::uint32_t>(c2 & 0x3Fu) << 6) |
           static_cast<std::uint32_t>(c3 & 0x3Fu);
      advance = 4;
    }
    units += (cp > 0xFFFFu) ? 2u : 1u;
    i += advance;
  }
  return units;
}

Value fast_string_builtin_value(const Value& input) {
  switch (input.kind) {
    case Value::Kind::String:
      return input;
    case Value::Kind::Int: {
      char buffer[32];
      auto* begin = buffer;
      auto* end = buffer + sizeof(buffer);
      const auto conv = std::to_chars(begin, end, input.int_value);
      if (conv.ec == std::errc{}) {
        return Value::string_value_of(std::string(begin, static_cast<std::size_t>(conv.ptr - begin)));
      }
      return Value::string_value_of(std::to_string(input.int_value));
    }
    case Value::Kind::Double:
      return Value::string_value_of(double_to_string(input.double_value));
    case Value::Kind::Numeric: {
      if (!input.numeric_value.has_value()) {
        return Value::string_value_of(input.to_string());
      }
      const auto& numeric = *input.numeric_value;
      if (numeric.parsed_int_valid) {
        return Value::string_value_of(numeric_int_to_string_fast(numeric));
      }
      if (numeric.parsed_float_valid && !numeric_kind_is_high_precision_float(numeric.kind)) {
        return Value::string_value_of(double_to_string(static_cast<double>(numeric.parsed_float)));
      }
      if (!numeric.payload.empty() &&
          (numeric.kind == Value::NumericKind::I256 || numeric.kind == Value::NumericKind::I512)) {
        return Value::string_value_of(numeric.payload);
      }
      return Value::string_value_of(input.to_string());
    }
    case Value::Kind::Bool:
      return Value::string_value_of(input.bool_value ? "True" : "False");
    default:
      return Value::string_value_of(input.to_string());
  }
}

Value fast_len_builtin_value(const Value& input) {
  if (input.kind == Value::Kind::List) {
    if (input.list_value.empty() &&
        input.list_cache.materialized_version == input.list_cache.version &&
        !input.list_cache.promoted_f64.empty()) {
      return Value::int_value_of(static_cast<long long>(input.list_cache.promoted_f64.size()));
    }
    return Value::int_value_of(static_cast<long long>(input.list_value.size()));
  }
  if (input.kind == Value::Kind::Matrix && input.matrix_value) {
    return Value::int_value_of(static_cast<long long>(input.matrix_value->rows));
  }
  if (input.kind == Value::Kind::String) {
    return Value::int_value_of(static_cast<long long>(fast_utf8_codepoint_count(input.string_value)));
  }
  throw EvalException("len() currently supports list, matrix, or string values");
}

Value fast_utf8_len_builtin_value(const Value& input) {
  if (input.kind != Value::Kind::String) {
    throw EvalException("utf8_len() expects exactly one string argument");
  }
  return Value::int_value_of(static_cast<long long>(input.string_value.size()));
}

Value fast_utf16_len_builtin_value(const Value& input) {
  if (input.kind != Value::Kind::String) {
    throw EvalException("utf16_len() expects exactly one string argument");
  }
  return Value::int_value_of(static_cast<long long>(fast_utf16_units(input.string_value)));
}

}  // namespace

Value evaluate_case_call(const CallExpr& call, Interpreter& self,
                        const std::shared_ptr<Environment>& env) {
  // Ultra-hot no-arg builtin call fast-path. This avoids generic callable
  // resolution/allocation overhead in tight loops (e.g. bench_tick probes).
  if (call.args.empty()) {
    if (const auto* builtin = try_lookup_builtin_callee(call, env); builtin && builtin->builtin_value) {
      switch (builtin->builtin_value->tag) {
        case Value::BuiltinTag::BenchTick:
          return fast_bench_tick_value();
        case Value::BuiltinTag::BenchTickRaw:
          return fast_bench_tick_raw_value();
        case Value::BuiltinTag::BenchTickScaleNum:
          return Value::int_value_of(fast_bench_tick_scale_num_value());
        case Value::BuiltinTag::BenchTickScaleDen:
          return Value::int_value_of(fast_bench_tick_scale_den_value());
        case Value::BuiltinTag::String:
          return Value::string_value_of("");
        default:
          break;
      }
      static const std::vector<Value> kNoArgs;
      return builtin->builtin_value->impl(kNoArgs);
    }
  }

  // One-arg numeric constructor fast-path without generic call dispatch.
  if (call.args.size() == 1) {
    if (const auto* builtin = try_lookup_builtin_callee(call, env); builtin && builtin->builtin_value) {
      const auto& builtin_name = builtin->builtin_value->name;
      if (builtin->builtin_value->numeric_constructor) {
        const auto target_kind = builtin->builtin_value->numeric_constructor_kind;
        std::string literal_text;
        bool literal_is_int = false;
        if (call.args[0] &&
            extract_numeric_literal_text(*call.args[0], literal_text, literal_is_int)) {
          struct NumericCtorLiteralCacheEntry {
            const CallExpr* expr = nullptr;
            std::uint64_t env_id = 0;
            const void* builtin_ptr = nullptr;
            Value value = Value::nil();
          };
          constexpr std::size_t kNumericCtorLiteralCacheSize = 1024;
          static thread_local std::array<NumericCtorLiteralCacheEntry, kNumericCtorLiteralCacheSize>
              cache{};
          const auto key_expr = &call;
          const auto key_env = env ? env->stable_id : 0;
          const auto key_builtin = static_cast<const void*>(builtin->builtin_value.get());
          const auto raw_hash =
              static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key_expr)) ^
              (static_cast<std::size_t>(key_env) * 11400714819323198485ull) ^
              static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(key_builtin));
          auto& slot = cache[raw_hash & (kNumericCtorLiteralCacheSize - 1)];
          if (slot.expr == key_expr && slot.env_id == key_env &&
              slot.builtin_ptr == key_builtin && is_numeric_kind(slot.value)) {
            return slot.value;
          }
          const auto source_kind =
              literal_is_int ? Value::NumericKind::I512 : Value::NumericKind::F512;
          const auto source = Value::numeric_value_of(source_kind, literal_text);
          slot.expr = key_expr;
          slot.env_id = key_env;
          slot.builtin_ptr = key_builtin;
          slot.value = cast_numeric_to_kind(target_kind, source);
          return slot.value;
        }
        Value value_storage;
        const Value* value_ref = try_lookup_global_variable_slot(call.args[0].get(), env);
        if (!value_ref && call.args[0] && call.args[0]->kind == Expr::Kind::Variable) {
          const auto& arg_var = static_cast<const VariableExpr&>(*call.args[0]);
          value_ref = env ? env->get_ptr(arg_var.name) : nullptr;
          if (!value_ref) {
            throw EvalException("undefined variable: " + arg_var.name);
          }
        }
        if (!value_ref) {
          value_storage = self.evaluate(*call.args[0], env);
          value_ref = &value_storage;
        }
        if (!is_numeric_kind(*value_ref)) {
          throw EvalException(builtin_name +
                              "() expects exactly one numeric argument");
        }
        return cast_numeric_to_kind(target_kind, *value_ref);
      }

      const auto tag = builtin->builtin_value->tag;
      if (tag == Value::BuiltinTag::String || tag == Value::BuiltinTag::Len ||
          tag == Value::BuiltinTag::Utf8Len || tag == Value::BuiltinTag::Utf16Len) {
        if (tag == Value::BuiltinTag::String) {
          I128 fast_i = 0;
          if (const auto* plan = fast_int_expr_plan_cached(call.args[0].get());
              plan && eval_fast_int_expr_plan(*plan, env, fast_i)) {
            return Value::string_value_of(i128_to_string_fast(fast_i));
          }
          if (try_eval_fast_int_expr_for_string(call.args[0].get(), env, fast_i)) {
            return Value::string_value_of(i128_to_string_fast(fast_i));
          }
        }
        Value value_storage;
        const Value* value_ref = try_lookup_global_variable_slot(call.args[0].get(), env);
        if (!value_ref && call.args[0] && call.args[0]->kind == Expr::Kind::Variable) {
          const auto& arg_var = static_cast<const VariableExpr&>(*call.args[0]);
          value_ref = env ? env->get_ptr(arg_var.name) : nullptr;
          if (!value_ref) {
            throw EvalException("undefined variable: " + arg_var.name);
          }
        }
        if (!value_ref) {
          value_storage = self.evaluate(*call.args[0], env);
          value_ref = &value_storage;
        }
        switch (tag) {
          case Value::BuiltinTag::String:
            return fast_string_builtin_value(*value_ref);
          case Value::BuiltinTag::Len:
            return fast_len_builtin_value(*value_ref);
          case Value::BuiltinTag::Utf8Len:
            return fast_utf8_len_builtin_value(*value_ref);
          case Value::BuiltinTag::Utf16Len:
            return fast_utf16_len_builtin_value(*value_ref);
          default:
            break;
        }
      }
    }
  }

  Value pipeline_result;
  if (try_execute_pipeline_call(call, self, env, pipeline_result)) {
    return pipeline_result;
  }

  auto callee = self.evaluate(*call.callee, env);

  if (callee.kind == Value::Kind::List || callee.kind == Value::Kind::Matrix ||
      callee.kind == Value::Kind::String) {
    if (call.args.empty()) {
      throw EvalException("index-call expects at least one index argument");
    }
    Value current = callee;
    for (const auto& arg_expr : call.args) {
      const auto index_value = self.evaluate(*arg_expr, env);
      const auto raw_index = value_to_int(index_value);

      if (current.kind == Value::Kind::Matrix) {
        const auto row = normalize_index_value(raw_index, matrix_row_count(current));
        if (row < 0 || static_cast<std::size_t>(row) >= matrix_row_count(current)) {
          throw EvalException("matrix index out of range");
        }
        current = matrix_row_as_list(current, row);
        continue;
      }

      if (current.kind == Value::Kind::String) {
        const auto normalized = normalize_index_value(raw_index, current.string_value.size());
        if (normalized < 0 || static_cast<std::size_t>(normalized) >= current.string_value.size()) {
          throw EvalException("index out of range");
        }
        std::string one_char;
        one_char.push_back(current.string_value[static_cast<std::size_t>(normalized)]);
        current = Value::string_value_of(std::move(one_char));
        continue;
      }

      if (current.kind != Value::Kind::List) {
        throw EvalException("index-call target is not list/matrix/string");
      }
      const auto normalized = normalize_index_value(raw_index, current.list_value.size());
      if (normalized < 0 || static_cast<std::size_t>(normalized) >= current.list_value.size()) {
        throw EvalException("index out of range");
      }
      Value next = current.list_value[static_cast<std::size_t>(normalized)];
      current = std::move(next);
    }
    return current;
  }

  // Fast-path numeric primitive constructors.
  // This avoids generic builtin dispatch overhead in hot loops while preserving
  // full literal precision for NumberExpr source text.
  if (callee.kind == Value::Kind::Builtin && callee.builtin_value && call.args.size() == 1) {
    if (callee.builtin_value->numeric_constructor) {
      const auto target_kind = callee.builtin_value->numeric_constructor_kind;
      std::string literal_text;
      bool literal_is_int = false;
      if (call.args[0] &&
          extract_numeric_literal_text(*call.args[0], literal_text, literal_is_int)) {
        const auto source_kind =
            literal_is_int ? Value::NumericKind::I512 : Value::NumericKind::F512;
        const auto source = Value::numeric_value_of(source_kind, literal_text);
        return cast_numeric_to_kind(target_kind, source);
      }
      Value value_storage;
      const Value* value_ref = try_lookup_global_variable_slot(call.args[0].get(), env);
      if (!value_ref && call.args[0] && call.args[0]->kind == Expr::Kind::Variable) {
        const auto& arg_var = static_cast<const VariableExpr&>(*call.args[0]);
        value_ref = env ? env->get_ptr(arg_var.name) : nullptr;
        if (!value_ref) {
          throw EvalException("undefined variable: " + arg_var.name);
        }
      }
      if (!value_ref) {
        value_storage = self.evaluate(*call.args[0], env);
        value_ref = &value_storage;
      }
      if (!is_numeric_kind(*value_ref)) {
        throw EvalException(callee.builtin_value->name + "() expects exactly one numeric argument");
      }
      return cast_numeric_to_kind(target_kind, *value_ref);
    }
  }

  auto& scratch = call_args_scratch();
  if (!scratch.in_use) {
    CallArgsScratchGuard guard(scratch);
    scratch.args.reserve(call.args.size());
    for (const auto& arg : call.args) {
      if (const auto* slot = try_lookup_global_variable_slot(arg.get(), env); slot) {
        scratch.args.push_back(*slot);
      } else {
        scratch.args.push_back(self.evaluate(*arg, env));
      }
    }
    if (callee.kind == Value::Kind::Class && callee.class_value) {
      return instantiate_class_value(callee, scratch.args);
    }
    if (callee.kind == Value::Kind::Function && callee.function_value &&
        callee.function_value->is_async) {
      return spawn_task_value(callee, scratch.args);
    }
    return invoke_callable_sync(callee, scratch.args);
  }

  std::vector<Value> args;
  args.reserve(call.args.size());
  for (const auto& arg : call.args) {
    if (const auto* slot = try_lookup_global_variable_slot(arg.get(), env); slot) {
      args.push_back(*slot);
    } else {
      args.push_back(self.evaluate(*arg, env));
    }
  }
  if (callee.kind == Value::Kind::Class && callee.class_value) {
    return instantiate_class_value(callee, args);
  }
  if (callee.kind == Value::Kind::Function && callee.function_value &&
      callee.function_value->is_async) {
    return spawn_task_value(callee, args);
  }
  return invoke_callable_sync(callee, args);
}

}  // namespace spark
