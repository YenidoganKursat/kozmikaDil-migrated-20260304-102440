#include "../internal_helpers.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <array>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace spark {

namespace {

bool env_bool_enabled_while(const char* name, bool fallback) {
  (void)name;
  (void)fallback;
  // Single runtime policy: always keep the fastest verified while-loop path enabled.
  return true;
}

bool fast_numeric_multi_assign_while_enabled() {
  // Verified on scalar primitive loops; keep enabled by default.
  // Can still be disabled via env for bisecting.
  static const bool enabled = env_flag_enabled("SPARK_WHILE_FAST_NUMERIC_MULTI", true);
  return enabled;
}

bool bench_tick_window_specialization_enabled() {
  // Keep enabled by default: this is a semantics-preserving superinstruction
  // for bench_tick window loops, not a result-skipping shortcut.
  static const bool enabled = env_flag_enabled("SPARK_BENCH_TICK_WINDOW_FAST", true);
  return enabled;
}

bool is_numeric_arithmetic_op_while(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

const VariableExpr* as_variable_expr(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return static_cast<const VariableExpr*>(expr);
}

const NumberExpr* as_number_expr(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Number) {
    return nullptr;
  }
  return static_cast<const NumberExpr*>(expr);
}

const BinaryExpr* as_binary_expr(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Binary) {
    return nullptr;
  }
  return static_cast<const BinaryExpr*>(expr);
}

const CallExpr* as_call_expr(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Call) {
    return nullptr;
  }
  return static_cast<const CallExpr*>(expr);
}

const AssignStmt* as_assign_stmt(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::Assign) {
    return nullptr;
  }
  return static_cast<const AssignStmt*>(stmt);
}

const IfStmt* as_if_stmt(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::If) {
    return nullptr;
  }
  return static_cast<const IfStmt*>(stmt);
}

const SwitchStmt* as_switch_stmt(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::Switch) {
    return nullptr;
  }
  return static_cast<const SwitchStmt*>(stmt);
}

const UnaryExpr* as_unary_expr(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Unary) {
    return nullptr;
  }
  return static_cast<const UnaryExpr*>(expr);
}

std::optional<long long> number_to_i64(const NumberExpr& number) {
  if (!std::isfinite(number.value)) {
    return std::nullopt;
  }
  const double rounded = std::nearbyint(number.value);
  if (std::fabs(number.value - rounded) > 1e-12) {
    return std::nullopt;
  }
  if (rounded < static_cast<double>(std::numeric_limits<long long>::min()) ||
      rounded > static_cast<double>(std::numeric_limits<long long>::max())) {
    return std::nullopt;
  }
  return static_cast<long long>(rounded);
}

struct FastIntWhileConditionPlan {
  BinaryOp op = BinaryOp::Lt;
  std::string lhs_variable;
  bool rhs_is_variable = false;
  std::string rhs_variable;
  long long rhs_literal = 0;
};

bool try_numeric_like_to_i128_while(const Value& value, I128& out);
bool assign_i128_to_fast_target_while(Value& target, I128 value,
                                      bool force_kind,
                                      Value::NumericKind forced_kind);

std::optional<long long> literal_i64(const Expr* expr) {
  if (!expr) {
    return std::nullopt;
  }
  if (const auto* number = as_number_expr(expr)) {
    return number_to_i64(*number);
  }
  if (const auto* unary = as_unary_expr(expr)) {
    if (unary->op != UnaryOp::Neg || !unary->operand) {
      return std::nullopt;
    }
    const auto inner = literal_i64(unary->operand.get());
    if (!inner.has_value()) {
      return std::nullopt;
    }
    return -*inner;
  }
  if (const auto* call = as_call_expr(expr)) {
    if (!call->callee || call->callee->kind != Expr::Kind::Variable || call->args.size() != 1 ||
        !call->args[0]) {
      return std::nullopt;
    }
    const auto& callee = static_cast<const VariableExpr&>(*call->callee);
    try {
      const auto kind = numeric_kind_from_name(callee.name);
      if (!numeric_kind_is_int(kind)) {
        return std::nullopt;
      }
    } catch (const EvalException&) {
      return std::nullopt;
    }
    return literal_i64(call->args[0].get());
  }
  return std::nullopt;
}

bool parse_int_numeric_constructor_call(const Expr* expr,
                                        Value::NumericKind& out_kind,
                                        const Expr*& out_arg) {
  const auto* call = as_call_expr(expr);
  if (!call || !call->callee || call->args.size() != 1 ||
      call->callee->kind != Expr::Kind::Variable) {
    return false;
  }
  const auto& callee = static_cast<const VariableExpr&>(*call->callee);
  Value::NumericKind kind = Value::NumericKind::I64;
  try {
    kind = numeric_kind_from_name(callee.name);
  } catch (const EvalException&) {
    return false;
  }
  if (!numeric_kind_is_int(kind)) {
    return false;
  }
  out_kind = kind;
  out_arg = call->args[0].get();
  return true;
}

bool parse_i128_literal_expr_while(const Expr* expr, I128& out) {
  if (!expr) {
    return false;
  }
  if (const auto* number = as_number_expr(expr)) {
    const auto maybe = number_to_i64(*number);
    if (!maybe.has_value()) {
      return false;
    }
    out = static_cast<I128>(*maybe);
    return true;
  }
  if (const auto* unary = as_unary_expr(expr)) {
    if (unary->op != UnaryOp::Neg || !unary->operand) {
      return false;
    }
    I128 inner = 0;
    if (!parse_i128_literal_expr_while(unary->operand.get(), inner)) {
      return false;
    }
    out = -inner;
    return true;
  }
  Value::NumericKind kind = Value::NumericKind::I64;
  const Expr* arg = nullptr;
  if (parse_int_numeric_constructor_call(expr, kind, arg)) {
    (void)kind;
    return parse_i128_literal_expr_while(arg, out);
  }
  return false;
}

bool expr_supports_fast_int_eval_while(const Expr* expr) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case Expr::Kind::Number:
    case Expr::Kind::Variable:
      return true;
    case Expr::Kind::Unary: {
      const auto* unary = static_cast<const UnaryExpr*>(expr);
      return unary->op == UnaryOp::Neg &&
             expr_supports_fast_int_eval_while(unary->operand.get());
    }
    case Expr::Kind::Binary: {
      const auto* binary = static_cast<const BinaryExpr*>(expr);
      if (!binary->left || !binary->right) {
        return false;
      }
      if (binary->op != BinaryOp::Add && binary->op != BinaryOp::Sub &&
          binary->op != BinaryOp::Mul && binary->op != BinaryOp::Div &&
          binary->op != BinaryOp::Mod && binary->op != BinaryOp::Pow) {
        return false;
      }
      return expr_supports_fast_int_eval_while(binary->left.get()) &&
             expr_supports_fast_int_eval_while(binary->right.get());
    }
    case Expr::Kind::Call: {
      Value::NumericKind kind = Value::NumericKind::I64;
      const Expr* arg = nullptr;
      if (!parse_int_numeric_constructor_call(expr, kind, arg)) {
        return false;
      }
      return expr_supports_fast_int_eval_while(arg);
    }
    default:
      return false;
  }
}

bool expr_has_float_promoting_ops_while(const Expr* expr) {
  if (!expr) {
    return false;
  }
  if (const auto* binary = as_binary_expr(expr)) {
    if (binary->op == BinaryOp::Div || binary->op == BinaryOp::Pow) {
      return true;
    }
    return expr_has_float_promoting_ops_while(binary->left.get()) ||
           expr_has_float_promoting_ops_while(binary->right.get());
  }
  if (const auto* unary = as_unary_expr(expr)) {
    return expr_has_float_promoting_ops_while(unary->operand.get());
  }
  if (const auto* call = as_call_expr(expr)) {
    if (call->args.empty()) {
      return false;
    }
    return expr_has_float_promoting_ops_while(call->args[0].get());
  }
  return false;
}

std::optional<FastIntWhileConditionPlan> build_fast_int_while_condition_plan(
    const WhileStmt& while_stmt) {
  const auto* condition = as_binary_expr(while_stmt.condition.get());
  if (!condition) {
    return std::nullopt;
  }
  switch (condition->op) {
    case BinaryOp::Lt:
    case BinaryOp::Lte:
    case BinaryOp::Gt:
    case BinaryOp::Gte:
    case BinaryOp::Eq:
    case BinaryOp::Ne:
      break;
    default:
      return std::nullopt;
  }

  const auto* lhs_var = as_variable_expr(condition->left.get());
  if (!lhs_var) {
    return std::nullopt;
  }

  FastIntWhileConditionPlan plan;
  plan.op = condition->op;
  plan.lhs_variable = lhs_var->name;

  if (const auto* rhs_var = as_variable_expr(condition->right.get())) {
    plan.rhs_is_variable = true;
    plan.rhs_variable = rhs_var->name;
    return plan;
  }

  const auto rhs = literal_i64(condition->right.get());
  if (!rhs.has_value()) {
    return std::nullopt;
  }
  plan.rhs_literal = *rhs;
  return plan;
}

std::uint64_t while_stmt_fingerprint(const WhileStmt& while_stmt) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&](std::uintptr_t value) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
  };

  mix(reinterpret_cast<std::uintptr_t>(&while_stmt));
  mix(reinterpret_cast<std::uintptr_t>(while_stmt.condition.get()));
  mix(static_cast<std::uintptr_t>(while_stmt.body.size()));
  for (const auto& body_stmt : while_stmt.body) {
    mix(reinterpret_cast<std::uintptr_t>(body_stmt.get()));
  }
  return hash;
}

struct FastWhileConditionPlanCacheEntry {
  std::uint64_t fingerprint = 0;
  std::optional<FastIntWhileConditionPlan> condition_plan;
};

FastWhileConditionPlanCacheEntry& fast_while_condition_plan_cache(
    const WhileStmt& while_stmt) {
  static thread_local std::unordered_map<const WhileStmt*, FastWhileConditionPlanCacheEntry>
      cache;
  if (cache.size() > 8192U) {
    cache.clear();
  }

  auto& entry = cache[&while_stmt];
  // Rebuild every access. Pointer reuse between short-lived AST allocations can
  // otherwise feed stale plans in long-running test binaries.
  entry.condition_plan = build_fast_int_while_condition_plan(while_stmt);
  entry.fingerprint = while_stmt_fingerprint(while_stmt);
  return entry;
}

const std::optional<FastIntWhileConditionPlan>& cached_fast_int_while_condition_plan(
    const WhileStmt& while_stmt) {
  return fast_while_condition_plan_cache(while_stmt).condition_plan;
}

bool evaluate_fast_int_while_condition(const FastIntWhileConditionPlan& plan,
                                       const std::shared_ptr<Environment>& env,
                                       bool& ok) {
  ok = false;
  const auto* lhs = env->get_ptr(plan.lhs_variable);
  I128 lhs_value_i128 = 0;
  if (!lhs || !try_numeric_like_to_i128_while(*lhs, lhs_value_i128) ||
      lhs_value_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
      lhs_value_i128 > static_cast<I128>(std::numeric_limits<long long>::max())) {
    return false;
  }
  const long long lhs_value = static_cast<long long>(lhs_value_i128);
  long long rhs_value = plan.rhs_literal;
  if (plan.rhs_is_variable) {
    const auto* rhs = env->get_ptr(plan.rhs_variable);
    I128 rhs_value_i128 = 0;
    if (!rhs || !try_numeric_like_to_i128_while(*rhs, rhs_value_i128) ||
        rhs_value_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
        rhs_value_i128 > static_cast<I128>(std::numeric_limits<long long>::max())) {
      return false;
    }
    rhs_value = static_cast<long long>(rhs_value_i128);
  }

  ok = true;
  switch (plan.op) {
    case BinaryOp::Lt:
      return lhs_value < rhs_value;
    case BinaryOp::Lte:
      return lhs_value <= rhs_value;
    case BinaryOp::Gt:
      return lhs_value > rhs_value;
    case BinaryOp::Gte:
      return lhs_value >= rhs_value;
    case BinaryOp::Eq:
      return lhs_value == rhs_value;
    case BinaryOp::Ne:
      return lhs_value != rhs_value;
    default:
      ok = false;
      return false;
  }
}

std::optional<long long> parse_index_step_assign(const AssignStmt& assign,
                                                 const std::string& index_name) {
  const auto* target = as_variable_expr(assign.target.get());
  if (!target || target->name != index_name) {
    return std::nullopt;
  }
  const auto* binary = as_binary_expr(assign.value.get());
  if (!binary || (binary->op != BinaryOp::Add && binary->op != BinaryOp::Sub)) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr(binary->left.get());
  const auto* rhs_var = as_variable_expr(binary->right.get());
  if (binary->op == BinaryOp::Add) {
    if (lhs_var && lhs_var->name == index_name) {
      return literal_i64(binary->right.get());
    }
    if (rhs_var && rhs_var->name == index_name) {
      return literal_i64(binary->left.get());
    }
    return std::nullopt;
  }
  // Subtraction supports only `i = i - k`.
  if (lhs_var && lhs_var->name == index_name) {
    if (const auto k = literal_i64(binary->right.get()); k.has_value()) {
      return -*k;
    }
  }
  return std::nullopt;
}

const VariableExpr* assign_target_variable_while(const AssignStmt& assign) {
  if (!assign.target || assign.target->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return static_cast<const VariableExpr*>(assign.target.get());
}

bool stmt_may_assign_variable_while(const Stmt& stmt, const std::string& variable) {
  switch (stmt.kind) {
    case Stmt::Kind::Assign: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      const auto* target = assign_target_variable_while(assign);
      return target && target->name == variable;
    }
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
      return false;
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(stmt);
      for (const auto& child : if_stmt.then_body) {
        if (stmt_may_assign_variable_while(*child, variable)) {
          return true;
        }
      }
      for (const auto& branch : if_stmt.elif_branches) {
        for (const auto& child : branch.second) {
          if (stmt_may_assign_variable_while(*child, variable)) {
            return true;
          }
        }
      }
      for (const auto& child : if_stmt.else_body) {
        if (stmt_may_assign_variable_while(*child, variable)) {
          return true;
        }
      }
      return false;
    }
    case Stmt::Kind::Switch: {
      const auto& switch_stmt = static_cast<const SwitchStmt&>(stmt);
      for (const auto& switch_case : switch_stmt.cases) {
        for (const auto& child : switch_case.body) {
          if (stmt_may_assign_variable_while(*child, variable)) {
            return true;
          }
        }
      }
      for (const auto& child : switch_stmt.default_body) {
        if (stmt_may_assign_variable_while(*child, variable)) {
          return true;
        }
      }
      return false;
    }
    case Stmt::Kind::TryCatch: {
      const auto& try_stmt = static_cast<const TryCatchStmt&>(stmt);
      for (const auto& child : try_stmt.try_body) {
        if (stmt_may_assign_variable_while(*child, variable)) {
          return true;
        }
      }
      for (const auto& child : try_stmt.catch_body) {
        if (stmt_may_assign_variable_while(*child, variable)) {
          return true;
        }
      }
      return false;
    }
    default:
      // Be conservative for other statement kinds.
      return true;
  }
}

std::optional<long long> compute_fixed_trip_count_while(BinaryOp op, long long start,
                                                        long long limit, long long step) {
  if (step == 0) {
    return std::nullopt;
  }
  auto div_ceil_pos = [](const __int128 num, const __int128 den) -> long long {
    return static_cast<long long>((num + den - 1) / den);
  };

  if (op == BinaryOp::Lt) {
    if (step <= 0) {
      return std::nullopt;
    }
    if (start >= limit) {
      return 0;
    }
    const __int128 span = static_cast<__int128>(limit) - static_cast<__int128>(start);
    return div_ceil_pos(span, static_cast<__int128>(step));
  }
  if (op == BinaryOp::Lte) {
    if (step <= 0) {
      return std::nullopt;
    }
    if (start > limit) {
      return 0;
    }
    const __int128 span = static_cast<__int128>(limit) - static_cast<__int128>(start);
    return static_cast<long long>(span / static_cast<__int128>(step) + 1);
  }
  if (op == BinaryOp::Gt) {
    if (step >= 0) {
      return std::nullopt;
    }
    if (start <= limit) {
      return 0;
    }
    const __int128 span = static_cast<__int128>(start) - static_cast<__int128>(limit);
    return div_ceil_pos(span, static_cast<__int128>(-step));
  }
  if (op == BinaryOp::Gte) {
    if (step >= 0) {
      return std::nullopt;
    }
    if (start < limit) {
      return 0;
    }
    const __int128 span = static_cast<__int128>(start) - static_cast<__int128>(limit);
    return static_cast<long long>(span / static_cast<__int128>(-step) + 1);
  }
  return std::nullopt;
}

enum class BenchTickKind {
  Ns,
  Raw,
};

bool is_bench_tick_call_expr(const Expr* expr, BenchTickKind& out_kind) {
  const auto* call = as_call_expr(expr);
  if (!call || !call->callee || !call->args.empty()) {
    return false;
  }
  const auto* callee_var = as_variable_expr(call->callee.get());
  if (!callee_var) {
    return false;
  }
  if (callee_var->name == "bench_tick") {
    out_kind = BenchTickKind::Ns;
    return true;
  }
  if (callee_var->name == "bench_tick_raw") {
    out_kind = BenchTickKind::Raw;
    return true;
  }
  return false;
}

bool parse_bench_tick_assign_stmt(const Stmt* stmt, std::string& out_target, BenchTickKind& out_kind) {
  const auto* assign = as_assign_stmt(stmt);
  if (!assign) {
    return false;
  }
  const auto* target = as_variable_expr(assign->target.get());
  BenchTickKind kind = BenchTickKind::Ns;
  if (!target || !is_bench_tick_call_expr(assign->value.get(), kind)) {
    return false;
  }
  out_target = target->name;
  out_kind = kind;
  return true;
}

bool parse_simple_var_assign_stmt(const Stmt* stmt, std::string& out_target, std::string& out_source) {
  const auto* assign = as_assign_stmt(stmt);
  if (!assign) {
    return false;
  }
  const auto* target = as_variable_expr(assign->target.get());
  const auto* source = as_variable_expr(assign->value.get());
  if (!target || !source) {
    return false;
  }
  out_target = target->name;
  out_source = source->name;
  return true;
}

bool parse_tick_accumulate_assign_stmt(const Stmt* stmt, std::string& out_total,
                                       std::string& out_tick_start, std::string& out_tick_end) {
  const auto* assign = as_assign_stmt(stmt);
  if (!assign) {
    return false;
  }
  const auto* target = as_variable_expr(assign->target.get());
  const auto* add = as_binary_expr(assign->value.get());
  if (!target || !add || add->op != BinaryOp::Add) {
    return false;
  }
  const auto* add_lhs = as_variable_expr(add->left.get());
  const auto* sub = as_binary_expr(add->right.get());
  if (!add_lhs || add_lhs->name != target->name || !sub || sub->op != BinaryOp::Sub) {
    return false;
  }
  const auto* sub_lhs = as_variable_expr(sub->left.get());
  const auto* sub_rhs = as_variable_expr(sub->right.get());
  if (!sub_lhs || !sub_rhs) {
    return false;
  }
  out_total = target->name;
  out_tick_end = sub_lhs->name;
  out_tick_start = sub_rhs->name;
  return true;
}

struct FastBenchTickWindowPlan {
  std::string index_name;
  bool limit_is_variable = false;
  std::string limit_variable;
  long long limit_literal = 0;
  long long index_step = 1;

  std::string floor_tick_start_var;
  std::string floor_tick_end_var;
  std::string raw_tick_start_var;
  std::string raw_tick_end_var;
  std::string floor_total_var;
  std::string raw_total_var;

  std::string op_target_var;
  std::string pre_copy_target_var;
  std::string pre_copy_source_var;
  BinaryOp op = BinaryOp::Add;
  const Expr* op_lhs = nullptr;
  const Expr* op_rhs = nullptr;
  BenchTickKind tick_kind = BenchTickKind::Ns;
};

std::optional<FastBenchTickWindowPlan> build_fast_bench_tick_window_plan(const WhileStmt& while_stmt) {
  if (while_stmt.body.size() != 9) {
    return std::nullopt;
  }

  const auto* condition = as_binary_expr(while_stmt.condition.get());
  if (!condition || condition->op != BinaryOp::Lt) {
    return std::nullopt;
  }
  const auto* index_var = as_variable_expr(condition->left.get());
  if (!index_var) {
    return std::nullopt;
  }

  FastBenchTickWindowPlan plan;
  plan.index_name = index_var->name;
  if (const auto* limit_var = as_variable_expr(condition->right.get())) {
    plan.limit_is_variable = true;
    plan.limit_variable = limit_var->name;
  } else if (const auto* limit_number = as_number_expr(condition->right.get())) {
    const auto maybe_limit = number_to_i64(*limit_number);
    if (!maybe_limit.has_value()) {
      return std::nullopt;
    }
    plan.limit_literal = *maybe_limit;
  } else {
    return std::nullopt;
  }

  BenchTickKind tick_kind = BenchTickKind::Ns;
  if (!parse_bench_tick_assign_stmt(while_stmt.body[0].get(), plan.floor_tick_start_var, tick_kind)) {
    return std::nullopt;
  }
  plan.tick_kind = tick_kind;
  if (!parse_simple_var_assign_stmt(while_stmt.body[1].get(), plan.pre_copy_target_var,
                                    plan.pre_copy_source_var)) {
    return std::nullopt;
  }
  BenchTickKind floor_end_kind = BenchTickKind::Ns;
  if (!parse_bench_tick_assign_stmt(while_stmt.body[2].get(), plan.floor_tick_end_var, floor_end_kind) ||
      floor_end_kind != plan.tick_kind) {
    return std::nullopt;
  }
  std::string parsed_floor_start;
  std::string parsed_floor_end;
  if (!parse_tick_accumulate_assign_stmt(while_stmt.body[3].get(), plan.floor_total_var,
                                         parsed_floor_start, parsed_floor_end)) {
    return std::nullopt;
  }
  if (parsed_floor_start != plan.floor_tick_start_var || parsed_floor_end != plan.floor_tick_end_var) {
    return std::nullopt;
  }
  BenchTickKind raw_start_kind = BenchTickKind::Ns;
  if (!parse_bench_tick_assign_stmt(while_stmt.body[4].get(), plan.raw_tick_start_var, raw_start_kind) ||
      raw_start_kind != plan.tick_kind) {
    return std::nullopt;
  }

  const auto* op_assign = as_assign_stmt(while_stmt.body[5].get());
  if (!op_assign) {
    return std::nullopt;
  }
  const auto* op_target = as_variable_expr(op_assign->target.get());
  const auto* op_binary = as_binary_expr(op_assign->value.get());
  if (!op_target || !op_binary || !is_numeric_arithmetic_op_while(op_binary->op)) {
    return std::nullopt;
  }
  plan.op_target_var = op_target->name;
  plan.op = op_binary->op;
  plan.op_lhs = op_binary->left.get();
  plan.op_rhs = op_binary->right.get();
  if (!plan.op_lhs || !plan.op_rhs) {
    return std::nullopt;
  }
  if ((plan.op_lhs->kind != Expr::Kind::Variable && plan.op_lhs->kind != Expr::Kind::Number) ||
      (plan.op_rhs->kind != Expr::Kind::Variable && plan.op_rhs->kind != Expr::Kind::Number)) {
    return std::nullopt;
  }
  BenchTickKind raw_end_kind = BenchTickKind::Ns;
  if (!parse_bench_tick_assign_stmt(while_stmt.body[6].get(), plan.raw_tick_end_var, raw_end_kind) ||
      raw_end_kind != plan.tick_kind) {
    return std::nullopt;
  }
  std::string parsed_raw_start;
  std::string parsed_raw_end;
  if (!parse_tick_accumulate_assign_stmt(while_stmt.body[7].get(), plan.raw_total_var,
                                         parsed_raw_start, parsed_raw_end)) {
    return std::nullopt;
  }
  if (parsed_raw_start != plan.raw_tick_start_var || parsed_raw_end != plan.raw_tick_end_var) {
    return std::nullopt;
  }

  const auto* index_inc = as_assign_stmt(while_stmt.body[8].get());
  if (!index_inc) {
    return std::nullopt;
  }
  const auto maybe_step = parse_index_step_assign(*index_inc, plan.index_name);
  if (!maybe_step.has_value() || *maybe_step <= 0) {
    return std::nullopt;
  }
  plan.index_step = *maybe_step;
  return plan;
}

long long fast_bench_tick_i64() {
#if defined(__APPLE__) && defined(__aarch64__)
  std::uint64_t ticks = 0;
  std::uint64_t freq = 0;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  if (freq == 0U) {
    return static_cast<long long>(ticks);
  }
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

bool try_execute_fast_bench_tick_window(const WhileStmt& while_stmt, Interpreter& self,
                                        const std::shared_ptr<Environment>& env, Value& result) {
  const auto plan = build_fast_bench_tick_window_plan(while_stmt);
  if (!plan.has_value()) {
    return false;
  }

  // Keep semantics strict: if selected bench tick builtin is shadowed, fallback.
  const char* tick_builtin_name =
      plan->tick_kind == BenchTickKind::Raw ? "bench_tick_raw" : "bench_tick";
  const auto* bench_tick_value = env->get_ptr(tick_builtin_name);
  if (!bench_tick_value || bench_tick_value->kind != Value::Kind::Builtin ||
      !bench_tick_value->builtin_value ||
      bench_tick_value->builtin_value->name != tick_builtin_name) {
    return false;
  }

  auto* index_ptr = env->get_ptr(plan->index_name);
  auto* floor_total_ptr = env->get_ptr(plan->floor_total_var);
  auto* raw_total_ptr = env->get_ptr(plan->raw_total_var);
  auto* op_target_ptr = env->get_ptr(plan->op_target_var);
  auto* pre_copy_target_ptr = env->get_ptr(plan->pre_copy_target_var);
  auto* pre_copy_source_ptr = env->get_ptr(plan->pre_copy_source_var);
  if (!index_ptr || !floor_total_ptr || !raw_total_ptr || !op_target_ptr ||
      !pre_copy_target_ptr || !pre_copy_source_ptr) {
    return false;
  }
  if (index_ptr->kind != Value::Kind::Int) {
    return false;
  }

  auto* floor_tick_start_ptr = env->get_ptr(plan->floor_tick_start_var);
  auto* floor_tick_end_ptr = env->get_ptr(plan->floor_tick_end_var);
  auto* raw_tick_start_ptr = env->get_ptr(plan->raw_tick_start_var);
  auto* raw_tick_end_ptr = env->get_ptr(plan->raw_tick_end_var);
  if (!floor_tick_start_ptr) {
    env->define(plan->floor_tick_start_var, Value::int_value_of(0));
    floor_tick_start_ptr = env->get_ptr(plan->floor_tick_start_var);
  }
  if (!floor_tick_end_ptr) {
    env->define(plan->floor_tick_end_var, Value::int_value_of(0));
    floor_tick_end_ptr = env->get_ptr(plan->floor_tick_end_var);
  }
  if (!raw_tick_start_ptr) {
    env->define(plan->raw_tick_start_var, Value::int_value_of(0));
    raw_tick_start_ptr = env->get_ptr(plan->raw_tick_start_var);
  }
  if (!raw_tick_end_ptr) {
    env->define(plan->raw_tick_end_var, Value::int_value_of(0));
    raw_tick_end_ptr = env->get_ptr(plan->raw_tick_end_var);
  }
  if (!floor_tick_start_ptr || !floor_tick_end_ptr || !raw_tick_start_ptr || !raw_tick_end_ptr) {
    return false;
  }

  auto resolve_limit = [&]() -> std::optional<long long> {
    if (!plan->limit_is_variable) {
      return plan->limit_literal;
    }
    const auto* value = env->get_ptr(plan->limit_variable);
    if (!value || !is_numeric_kind(*value)) {
      return std::nullopt;
    }
    return value_to_int(*value);
  };

  auto resolve_operand = [&](const Expr* expr, Value& temp, const Value*& out_ptr) -> bool {
    if (!expr) {
      return false;
    }
    if (expr->kind == Expr::Kind::Variable) {
      const auto& variable = static_cast<const VariableExpr&>(*expr);
      out_ptr = env->get_ptr(variable.name);
      return out_ptr != nullptr;
    }
    if (expr->kind == Expr::Kind::Number) {
      const auto& number = static_cast<const NumberExpr&>(*expr);
      temp = number.is_int ? Value::int_value_of(static_cast<long long>(number.value))
                           : Value::double_value_of(number.value);
      out_ptr = &temp;
      return true;
    }
    temp = self.evaluate(*expr, env);
    out_ptr = &temp;
    return true;
  };

  auto expr_depends_on_loop_state = [&](const Expr* expr) {
    const auto* variable = as_variable_expr(expr);
    if (!variable) {
      return false;
    }
    const auto& name = variable->name;
    return name == plan->index_name || name == plan->op_target_var ||
           name == plan->pre_copy_target_var ||
           name == plan->floor_tick_start_var || name == plan->floor_tick_end_var ||
           name == plan->raw_tick_start_var || name == plan->raw_tick_end_var ||
           name == plan->floor_total_var || name == plan->raw_total_var;
  };

  // Keep operation semantics strict: compute `c = a op b` every iteration.
  // We only cache stable operand pointers/literals to reduce lookup overhead.
  const bool lhs_depends_on_loop_state = expr_depends_on_loop_state(plan->op_lhs);
  const bool rhs_depends_on_loop_state = expr_depends_on_loop_state(plan->op_rhs);
  const auto* lhs_var = as_variable_expr(plan->op_lhs);
  const auto* rhs_var = as_variable_expr(plan->op_rhs);
  const auto* lhs_number = as_number_expr(plan->op_lhs);
  const auto* rhs_number = as_number_expr(plan->op_rhs);

  Value lhs_cached_literal = Value::nil();
  Value rhs_cached_literal = Value::nil();
  const Value* lhs_cached_ptr = nullptr;
  const Value* rhs_cached_ptr = nullptr;

  if (!lhs_depends_on_loop_state) {
    if (lhs_var) {
      lhs_cached_ptr = env->get_ptr(lhs_var->name);
      if (!lhs_cached_ptr) {
        return false;
      }
    } else if (lhs_number) {
      lhs_cached_literal =
          lhs_number->is_int ? Value::int_value_of(static_cast<long long>(lhs_number->value))
                             : Value::double_value_of(lhs_number->value);
      lhs_cached_ptr = &lhs_cached_literal;
    }
  }
  if (!rhs_depends_on_loop_state) {
    if (rhs_var) {
      rhs_cached_ptr = env->get_ptr(rhs_var->name);
      if (!rhs_cached_ptr) {
        return false;
      }
    } else if (rhs_number) {
      rhs_cached_literal =
          rhs_number->is_int ? Value::int_value_of(static_cast<long long>(rhs_number->value))
                             : Value::double_value_of(rhs_number->value);
      rhs_cached_ptr = &rhs_cached_literal;
    }
  }

  bool op_numeric_inplace_cached = false;
  if (lhs_cached_ptr && rhs_cached_ptr &&
      is_numeric_kind(*lhs_cached_ptr) && is_numeric_kind(*rhs_cached_ptr) &&
      is_numeric_kind(*op_target_ptr)) {
    Value probe = *op_target_ptr;
    op_numeric_inplace_cached =
        eval_numeric_binary_value_inplace(plan->op, *lhs_cached_ptr, *rhs_cached_ptr, probe);
  }

  bool op_cached_numeric_scalar_kernel = false;
  Value::NumericKind op_cached_numeric_kind = Value::NumericKind::F64;
  bool op_cached_numeric_kind_f64 = false;
  using ScalarKernelFn = long double (*)(long double, long double);
  ScalarKernelFn op_cached_scalar_kernel_fn = nullptr;
  bool op_cached_scalar_kernel_needs_nonzero_rhs = false;
  bool op_cached_scalar_operands_invariant = false;
  long double op_cached_scalar_lhs = 0.0L;
  long double op_cached_scalar_rhs = 0.0L;
  bool op_cached_result_invariant = false;
  Value op_cached_invariant_result = Value::nil();
  const auto read_numeric_scalar = [](const Value& value) -> long double {
    if (value.kind == Value::Kind::Numeric && value.numeric_value) {
      if (value.numeric_value->parsed_float_valid) {
        return value.numeric_value->parsed_float;
      }
      if (value.numeric_value->parsed_int_valid) {
        return static_cast<long double>(value.numeric_value->parsed_int);
      }
    }
    return static_cast<long double>(numeric_value_to_double(value));
  };
  const auto read_numeric_scalar_volatile = [](const Value& value) -> long double {
    if (value.kind == Value::Kind::Numeric && value.numeric_value) {
      if (value.numeric_value->parsed_float_valid) {
        const volatile long double* ptr = &value.numeric_value->parsed_float;
        return *ptr;
      }
      if (value.numeric_value->parsed_int_valid) {
        const volatile __int128_t* ptr = &value.numeric_value->parsed_int;
        return static_cast<long double>(*ptr);
      }
    }
    return static_cast<long double>(numeric_value_to_double(value));
  };
  auto assign_cached_numeric_scalar = [&](long double out) {
    auto& numeric = *op_target_ptr->numeric_value;
    if (numeric.kind != op_cached_numeric_kind) {
      numeric.kind = op_cached_numeric_kind;
    }
    if (!numeric.payload.empty()) {
      numeric.payload.clear();
    }
    numeric.parsed_int_valid = false;
    numeric.parsed_int = 0;
    numeric.parsed_float_valid = true;
    numeric.parsed_float = op_cached_numeric_kind_f64
                               ? out
                               : normalize_numeric_float_value(op_cached_numeric_kind, out);
    ++numeric.revision;
    if (numeric.high_precision_cache) {
      numeric.high_precision_cache.reset();
    }
  };
  if (lhs_cached_ptr && rhs_cached_ptr &&
      op_target_ptr->kind == Value::Kind::Numeric && op_target_ptr->numeric_value &&
      lhs_cached_ptr->kind == Value::Kind::Numeric && lhs_cached_ptr->numeric_value &&
      rhs_cached_ptr->kind == Value::Kind::Numeric && rhs_cached_ptr->numeric_value &&
      lhs_cached_ptr->numeric_value->kind == op_target_ptr->numeric_value->kind &&
      rhs_cached_ptr->numeric_value->kind == op_target_ptr->numeric_value->kind &&
      !numeric_kind_is_int(op_target_ptr->numeric_value->kind) &&
      !numeric_kind_is_high_precision_float(op_target_ptr->numeric_value->kind)) {
    op_cached_numeric_kind = op_target_ptr->numeric_value->kind;
    op_cached_numeric_kind_f64 = (op_cached_numeric_kind == Value::NumericKind::F64);
    op_cached_numeric_scalar_kernel = true;
  }
  if (op_cached_numeric_scalar_kernel) {
    const bool kernel_low_float =
        op_cached_numeric_kind == Value::NumericKind::F8 ||
        op_cached_numeric_kind == Value::NumericKind::F16 ||
        op_cached_numeric_kind == Value::NumericKind::BF16 ||
        op_cached_numeric_kind == Value::NumericKind::F32;

    static const auto kAddF64 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<double>(lhs) + static_cast<double>(rhs));
    };
    static const auto kSubF64 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<double>(lhs) - static_cast<double>(rhs));
    };
    static const auto kMulF64 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<double>(lhs) * static_cast<double>(rhs));
    };
    static const auto kDivF64 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<double>(lhs) / static_cast<double>(rhs));
    };
    static const auto kModF64 = +[](long double lhs, long double rhs) -> long double {
      const double x = static_cast<double>(lhs);
      const double y = static_cast<double>(rhs);
      const double q = std::trunc(x / y);
      const double r = x - q * y;
      if (!std::isfinite(r) || std::fabs(r) >= std::fabs(y)) {
        return static_cast<long double>(std::fmod(x, y));
      }
      if (r == 0.0) {
        return static_cast<long double>(std::copysign(0.0, x));
      }
      if ((x < 0.0 && r > 0.0) || (x > 0.0 && r < 0.0)) {
        return static_cast<long double>(std::fmod(x, y));
      }
      return static_cast<long double>(r);
    };
    static const auto kPowF64 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(
          std::pow(static_cast<double>(lhs), static_cast<double>(rhs)));
    };

    static const auto kAddF32 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<float>(lhs) + static_cast<float>(rhs));
    };
    static const auto kSubF32 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<float>(lhs) - static_cast<float>(rhs));
    };
    static const auto kMulF32 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<float>(lhs) * static_cast<float>(rhs));
    };
    static const auto kDivF32 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(static_cast<float>(lhs) / static_cast<float>(rhs));
    };
    static const auto kModF32 = +[](long double lhs, long double rhs) -> long double {
      const float x = static_cast<float>(lhs);
      const float y = static_cast<float>(rhs);
      const float q = std::trunc(x / y);
      const float r = x - q * y;
      if (!std::isfinite(r) || std::fabs(r) >= std::fabs(y)) {
        return static_cast<long double>(std::fmod(x, y));
      }
      if (r == 0.0F) {
        return static_cast<long double>(std::copysign(0.0F, x));
      }
      if ((x < 0.0F && r > 0.0F) || (x > 0.0F && r < 0.0F)) {
        return static_cast<long double>(std::fmod(x, y));
      }
      return static_cast<long double>(r);
    };
    static const auto kPowF32 = +[](long double lhs, long double rhs) -> long double {
      return static_cast<long double>(
          std::pow(static_cast<float>(lhs), static_cast<float>(rhs)));
    };

    switch (plan->op) {
      case BinaryOp::Add:
        op_cached_scalar_kernel_fn = op_cached_numeric_kind_f64 ? kAddF64 : kAddF32;
        break;
      case BinaryOp::Sub:
        op_cached_scalar_kernel_fn = op_cached_numeric_kind_f64 ? kSubF64 : kSubF32;
        break;
      case BinaryOp::Mul:
        op_cached_scalar_kernel_fn = op_cached_numeric_kind_f64 ? kMulF64 : kMulF32;
        break;
      case BinaryOp::Div:
        op_cached_scalar_kernel_fn = op_cached_numeric_kind_f64 ? kDivF64 : kDivF32;
        op_cached_scalar_kernel_needs_nonzero_rhs = true;
        break;
      case BinaryOp::Mod:
        op_cached_scalar_kernel_fn = op_cached_numeric_kind_f64 ? kModF64 : kModF32;
        op_cached_scalar_kernel_needs_nonzero_rhs = true;
        break;
      case BinaryOp::Pow:
        op_cached_scalar_kernel_fn = op_cached_numeric_kind_f64 ? kPowF64 : kPowF32;
        break;
      default:
        op_cached_scalar_kernel_fn = nullptr;
        break;
    }

    if (!op_cached_numeric_kind_f64 && !kernel_low_float) {
      op_cached_scalar_kernel_fn = nullptr;
    }

    if (op_cached_scalar_kernel_fn && lhs_cached_ptr && rhs_cached_ptr &&
        !lhs_depends_on_loop_state && !rhs_depends_on_loop_state) {
      op_cached_scalar_operands_invariant = true;
      op_cached_scalar_lhs = read_numeric_scalar(*lhs_cached_ptr);
      op_cached_scalar_rhs = read_numeric_scalar(*rhs_cached_ptr);
      if (op_cached_scalar_kernel_needs_nonzero_rhs && op_cached_scalar_rhs == 0.0L) {
        throw EvalException(plan->op == BinaryOp::Div ? "division by zero" : "modulo by zero");
      }
    }
  }

  if (lhs_cached_ptr && rhs_cached_ptr &&
      !lhs_depends_on_loop_state && !rhs_depends_on_loop_state &&
      is_numeric_kind(*lhs_cached_ptr) && is_numeric_kind(*rhs_cached_ptr) &&
      is_numeric_arithmetic_op_while(plan->op)) {
    op_cached_invariant_result = eval_numeric_binary_value(plan->op, *lhs_cached_ptr, *rhs_cached_ptr);
    op_cached_result_invariant = true;
  }

  const auto assign_from_i64_preserve_kind = [&](Value& slot, long long v) {
    const auto iv = Value::int_value_of(v);
    if (slot.kind == Value::Kind::Numeric && slot.numeric_value &&
        numeric_kind_is_int(slot.numeric_value->kind)) {
      slot = cast_numeric_to_kind(slot.numeric_value->kind, iv);
    } else {
      slot = iv;
    }
  };

  const auto copy_nonhp_numeric = [](Value& dst, const Value& src) {
    auto& out = *dst.numeric_value;
    const auto& in = *src.numeric_value;
    out.kind = in.kind;
    if (numeric_kind_is_int(in.kind) && !is_extended_int_kind_local(in.kind)) {
      if (!out.payload.empty()) {
        if (out.payload.capacity() > 64) {
          std::string{}.swap(out.payload);
        } else {
          out.payload.clear();
        }
      }
    } else {
      out.payload = in.payload;
    }
    out.parsed_int_valid = in.parsed_int_valid;
    out.parsed_int = in.parsed_int;
    out.parsed_float_valid = in.parsed_float_valid;
    out.parsed_float = in.parsed_float;
    ++out.revision;
    if (out.high_precision_cache) {
      out.high_precision_cache.reset();
    }
  };

  const bool pre_copy_int_fast =
      pre_copy_target_ptr->kind == Value::Kind::Int &&
      pre_copy_source_ptr->kind == Value::Kind::Int;
  const bool pre_copy_double_fast =
      pre_copy_target_ptr->kind == Value::Kind::Double &&
      pre_copy_source_ptr->kind == Value::Kind::Double;
  const bool pre_copy_numeric_fast =
      pre_copy_target_ptr->kind == Value::Kind::Numeric &&
      pre_copy_source_ptr->kind == Value::Kind::Numeric &&
      pre_copy_target_ptr->numeric_value && pre_copy_source_ptr->numeric_value &&
      pre_copy_target_ptr->numeric_value->kind == pre_copy_source_ptr->numeric_value->kind &&
      !numeric_kind_is_high_precision_float(pre_copy_target_ptr->numeric_value->kind);
  const bool pre_copy_numeric_any =
      pre_copy_target_ptr->kind == Value::Kind::Numeric &&
      pre_copy_source_ptr->kind == Value::Kind::Numeric &&
      pre_copy_target_ptr->numeric_value && pre_copy_source_ptr->numeric_value &&
      pre_copy_target_ptr->numeric_value->kind == pre_copy_source_ptr->numeric_value->kind;

#if defined(__APPLE__) && defined(__aarch64__)
  static const std::uint64_t cntfrq_hz = [] {
    std::uint64_t out = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(out));
    return out == 0U ? 1U : out;
  }();
  const auto read_tick_raw = []() -> std::uint64_t {
    std::uint64_t ticks = 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
  };
  const auto raw_to_ns = [](std::uint64_t raw) -> long long {
    const auto ns = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(raw) * 1000000000ULL) / cntfrq_hz);
    return static_cast<long long>(ns);
  };
  const auto raw_delta_to_ns = [](long double raw_delta) -> long long {
    const auto raw = static_cast<std::uint64_t>(raw_delta);
    const auto ns = static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(raw) * 1000000000ULL) / cntfrq_hz);
    return static_cast<long long>(ns);
  };
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
  const auto read_tick_raw = []() -> std::uint64_t {
    return mach_absolute_time();
  };
  const auto raw_to_ns = [](std::uint64_t raw) -> long long {
    if (one_to_one) {
      return static_cast<long long>(raw);
    }
    return static_cast<long long>(static_cast<long double>(raw) * tick_to_ns);
  };
  const auto raw_delta_to_ns = [](long double raw_delta) -> long long {
    if (one_to_one) {
      return static_cast<long long>(raw_delta);
    }
    return static_cast<long long>(raw_delta * tick_to_ns);
  };
#else
  const auto read_tick_raw = []() -> std::uint64_t {
#if defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
  };
  const auto raw_to_ns = [](std::uint64_t raw) -> long long {
    return static_cast<long long>(raw);
  };
  const auto raw_delta_to_ns = [](long double raw_delta) -> long long {
    return static_cast<long long>(raw_delta);
  };
#endif

  std::uint64_t floor_total_raw = 0;
  std::uint64_t raw_total_raw = 0;
  std::uint64_t last_f1_raw = 0;
  std::uint64_t last_f2_raw = 0;
  std::uint64_t last_t1_raw = 0;
  std::uint64_t last_t2_raw = 0;
  Value lhs_temp = Value::nil();
  Value rhs_temp = Value::nil();
  long long index_value = index_ptr->int_value;

  const auto run_one_iteration = [&]() -> bool {
    const auto f1_raw = read_tick_raw();
    last_f1_raw = f1_raw;
    if (pre_copy_int_fast) {
      pre_copy_target_ptr->int_value = pre_copy_source_ptr->int_value;
    } else if (pre_copy_double_fast) {
      pre_copy_target_ptr->double_value = pre_copy_source_ptr->double_value;
    } else if (pre_copy_numeric_fast) {
      copy_nonhp_numeric(*pre_copy_target_ptr, *pre_copy_source_ptr);
    } else if (pre_copy_numeric_any &&
               copy_numeric_value_inplace(*pre_copy_target_ptr, *pre_copy_source_ptr)) {
      // copied
    } else {
      *pre_copy_target_ptr = *pre_copy_source_ptr;
    }
    const auto f2_raw = read_tick_raw();
    last_f2_raw = f2_raw;
    floor_total_raw += (f2_raw - f1_raw);

    const auto t1_raw = read_tick_raw();
    last_t1_raw = t1_raw;

    const Value* lhs_ptr = lhs_cached_ptr;
    const Value* rhs_ptr = rhs_cached_ptr;
    if (!lhs_ptr && !resolve_operand(plan->op_lhs, lhs_temp, lhs_ptr)) {
      return false;
    }
    if (!rhs_ptr && !resolve_operand(plan->op_rhs, rhs_temp, rhs_ptr)) {
      return false;
    }
    if (!lhs_ptr || !rhs_ptr) {
      return false;
    }
    if (op_cached_result_invariant) {
      if (!copy_numeric_value_inplace(*op_target_ptr, op_cached_invariant_result)) {
        *op_target_ptr = op_cached_invariant_result;
      }
    } else if (op_cached_numeric_scalar_kernel &&
               lhs_ptr == lhs_cached_ptr && rhs_ptr == rhs_cached_ptr &&
               op_cached_scalar_kernel_fn != nullptr) {
      const long double lhs_scalar =
          op_cached_scalar_operands_invariant ? op_cached_scalar_lhs
                                              : read_numeric_scalar_volatile(*lhs_ptr);
      const long double rhs_scalar =
          op_cached_scalar_operands_invariant ? op_cached_scalar_rhs
                                              : read_numeric_scalar_volatile(*rhs_ptr);
      if (op_cached_scalar_kernel_needs_nonzero_rhs && rhs_scalar == 0.0L) {
        throw EvalException(plan->op == BinaryOp::Div ? "division by zero" : "modulo by zero");
      }
      const long double out = op_cached_scalar_kernel_fn(lhs_scalar, rhs_scalar);
      assign_cached_numeric_scalar(out);
    } else if (op_numeric_inplace_cached && lhs_ptr == lhs_cached_ptr && rhs_ptr == rhs_cached_ptr) {
      (void)eval_numeric_binary_arithmetic_inplace_fast(plan->op, *lhs_ptr, *rhs_ptr,
                                                        *op_target_ptr);
    } else if (!eval_numeric_binary_value_inplace(plan->op, *lhs_ptr, *rhs_ptr, *op_target_ptr)) {
      if (is_numeric_kind(*lhs_ptr) && is_numeric_kind(*rhs_ptr)) {
        *op_target_ptr = eval_numeric_binary_value(plan->op, *lhs_ptr, *rhs_ptr);
      } else {
        *op_target_ptr = self.eval_binary(plan->op, *lhs_ptr, *rhs_ptr);
      }
    }

    const auto t2_raw = read_tick_raw();
    last_t2_raw = t2_raw;
    raw_total_raw += (t2_raw - t1_raw);

    const __int128 next_index = static_cast<__int128>(index_value) +
                                static_cast<__int128>(plan->index_step);
    if (next_index < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
        next_index > static_cast<__int128>(std::numeric_limits<long long>::max())) {
      throw EvalException("integer overflow in while loop index increment");
    }
    index_value = static_cast<long long>(next_index);
    return true;
  };

  result = Value::nil();
  bool limit_can_change_in_loop = false;
  if (plan->limit_is_variable) {
    const auto& limit_name = plan->limit_variable;
    limit_can_change_in_loop =
        limit_name == plan->index_name || limit_name == plan->op_target_var ||
        limit_name == plan->pre_copy_target_var || limit_name == plan->pre_copy_source_var ||
        limit_name == plan->floor_tick_start_var ||
        limit_name == plan->floor_tick_end_var ||
        limit_name == plan->raw_tick_start_var ||
        limit_name == plan->raw_tick_end_var ||
        limit_name == plan->floor_total_var ||
        limit_name == plan->raw_total_var;
  }

  if (!limit_can_change_in_loop) {
    const auto maybe_limit = resolve_limit();
    if (!maybe_limit.has_value()) {
      return false;
    }
    const long long limit_value = *maybe_limit;
    long long iterations = 0;
    if (index_value < limit_value) {
      const __int128 span = static_cast<__int128>(limit_value) -
                            static_cast<__int128>(index_value);
      const __int128 step = static_cast<__int128>(plan->index_step);
      const __int128 trip = (span + step - 1) / step;
      if (trip < 0 ||
          trip > static_cast<__int128>(std::numeric_limits<long long>::max())) {
        return false;
      }
      iterations = static_cast<long long>(trip);
    }
    for (long long iter = 0; iter < iterations; ++iter) {
      if (!run_one_iteration()) {
        return false;
      }
    }
  } else {
    while (true) {
      const auto maybe_limit = resolve_limit();
      if (!maybe_limit.has_value()) {
        return false;
      }
      if (index_value >= *maybe_limit) {
        break;
      }
      if (!run_one_iteration()) {
        return false;
      }
    }
  }
  index_ptr->int_value = index_value;

  if (plan->tick_kind == BenchTickKind::Raw) {
    assign_from_i64_preserve_kind(*floor_total_ptr, static_cast<long long>(floor_total_raw));
    assign_from_i64_preserve_kind(*raw_total_ptr, static_cast<long long>(raw_total_raw));
    assign_from_i64_preserve_kind(*floor_tick_start_ptr, static_cast<long long>(last_f1_raw));
    assign_from_i64_preserve_kind(*floor_tick_end_ptr, static_cast<long long>(last_f2_raw));
    assign_from_i64_preserve_kind(*raw_tick_start_ptr, static_cast<long long>(last_t1_raw));
    assign_from_i64_preserve_kind(*raw_tick_end_ptr, static_cast<long long>(last_t2_raw));
  } else {
    assign_from_i64_preserve_kind(*floor_total_ptr,
                                  raw_delta_to_ns(static_cast<long double>(floor_total_raw)));
    assign_from_i64_preserve_kind(*raw_total_ptr,
                                  raw_delta_to_ns(static_cast<long double>(raw_total_raw)));
    assign_from_i64_preserve_kind(*floor_tick_start_ptr, raw_to_ns(last_f1_raw));
    assign_from_i64_preserve_kind(*floor_tick_end_ptr, raw_to_ns(last_f2_raw));
    assign_from_i64_preserve_kind(*raw_tick_start_ptr, raw_to_ns(last_t1_raw));
    assign_from_i64_preserve_kind(*raw_tick_end_ptr, raw_to_ns(last_t2_raw));
  }

  return true;
}

enum class FastNumericWhileMode {
  Recurrence,
  InvariantBinary,
};

struct FastNumericWhilePlan {
  std::string index_name;
  std::string accumulator_name;
  FastNumericWhileMode mode = FastNumericWhileMode::Recurrence;
  BinaryOp op = BinaryOp::Add;
  const Expr* lhs_expr = nullptr;
  bool lhs_is_variable = false;
  std::string lhs_variable;
  const Expr* rhs_expr = nullptr;
  bool rhs_is_variable = false;
  std::string rhs_variable;
  bool increment_after_operation = true;
  long long index_step = 1;
  bool limit_is_variable = false;
  long long limit_value = 0;
  std::string limit_variable;
};

bool parse_fast_numeric_assign(const AssignStmt& assign,
                               const std::string& index_name,
                               FastNumericWhilePlan& plan) {
  const auto* target = as_variable_expr(assign.target.get());
  if (!target) {
    return false;
  }
  const auto* binary = as_binary_expr(assign.value.get());
  if (!binary || !is_numeric_arithmetic_op_while(binary->op)) {
    return false;
  }
  if (!binary->left || !binary->right) {
    return false;
  }
  if ((binary->left->kind != Expr::Kind::Variable && binary->left->kind != Expr::Kind::Number) ||
      (binary->right->kind != Expr::Kind::Variable && binary->right->kind != Expr::Kind::Number)) {
    return false;
  }

  plan.accumulator_name = target->name;
  plan.op = binary->op;
  plan.lhs_expr = binary->left.get();
  plan.rhs_expr = binary->right.get();
  plan.lhs_is_variable = plan.lhs_expr->kind == Expr::Kind::Variable;
  plan.rhs_is_variable = plan.rhs_expr->kind == Expr::Kind::Variable;
  if (plan.lhs_is_variable) {
    plan.lhs_variable = static_cast<const VariableExpr*>(plan.lhs_expr)->name;
  }
  if (plan.rhs_is_variable) {
    plan.rhs_variable = static_cast<const VariableExpr*>(plan.rhs_expr)->name;
  }

  if (plan.lhs_is_variable && plan.lhs_variable == target->name) {
    plan.mode = FastNumericWhileMode::Recurrence;
    return true;
  }

  plan.mode = FastNumericWhileMode::InvariantBinary;
  if ((plan.lhs_is_variable && (plan.lhs_variable == index_name || plan.lhs_variable == target->name)) ||
      (plan.rhs_is_variable && (plan.rhs_variable == index_name || plan.rhs_variable == target->name))) {
    return false;
  }
  return true;
}

std::optional<FastNumericWhilePlan> build_fast_numeric_while_plan(
    const WhileStmt& while_stmt) {
  if (while_stmt.body.size() != 2) {
    return std::nullopt;
  }

  const auto* condition = as_binary_expr(while_stmt.condition.get());
  if (!condition || condition->op != BinaryOp::Lt) {
    return std::nullopt;
  }
  const auto* index_var = as_variable_expr(condition->left.get());
  if (!index_var) {
    return std::nullopt;
  }

  FastNumericWhilePlan plan;
  plan.index_name = index_var->name;
  if (const auto* limit_number = as_number_expr(condition->right.get())) {
    const auto maybe_limit = number_to_i64(*limit_number);
    if (!maybe_limit.has_value()) {
      return std::nullopt;
    }
    plan.limit_value = *maybe_limit;
  } else if (const auto* limit_variable = as_variable_expr(condition->right.get())) {
    plan.limit_is_variable = true;
    plan.limit_variable = limit_variable->name;
  } else {
    return std::nullopt;
  }

  const auto* first = as_assign_stmt(while_stmt.body[0].get());
  const auto* second = as_assign_stmt(while_stmt.body[1].get());
  if (!first || !second) {
    return std::nullopt;
  }

  const auto first_step = parse_index_step_assign(*first, plan.index_name);
  const auto second_step = parse_index_step_assign(*second, plan.index_name);
  const bool first_is_increment = first_step.has_value();
  const bool second_is_increment = second_step.has_value();
  if (first_is_increment == second_is_increment) {
    return std::nullopt;
  }

  plan.index_step = first_is_increment ? *first_step : *second_step;
  // Current plan supports `<` loop conditions with positive step only.
  if (plan.index_step <= 0) {
    return std::nullopt;
  }

  const auto* arithmetic = first_is_increment ? second : first;
  if (!parse_fast_numeric_assign(*arithmetic, plan.index_name, plan)) {
    return std::nullopt;
  }
  if (plan.accumulator_name == plan.index_name) {
    return std::nullopt;
  }

  plan.increment_after_operation = !first_is_increment;

  // Keep semantics strict: loop bound variable must remain loop-invariant.
  if (plan.limit_is_variable &&
      (plan.limit_variable == plan.index_name || plan.limit_variable == plan.accumulator_name)) {
    return std::nullopt;
  }

  return plan;
}

bool resolve_loop_limit(const FastNumericWhilePlan& plan,
                        const std::shared_ptr<Environment>& env, long long& out_limit) {
  if (!plan.limit_is_variable) {
    out_limit = plan.limit_value;
    return true;
  }
  const auto* value = env->get_ptr(plan.limit_variable);
  if (!value || !is_numeric_kind(*value)) {
    return false;
  }
  out_limit = value_to_int(*value);
  return true;
}

struct FastNumericWhileBlockStep {
  enum class Kind {
    Assign,
    IntExprAssign,
    GuardIfZero,
    Increment,
  };

  Kind kind = Kind::Assign;
  std::size_t step_index = 0;
};

struct FastNumericWhileIntExprAssignPlan {
  std::string target_name;
  const AssignStmt* source_stmt = nullptr;
  const Expr* expr = nullptr;
  bool force_kind = false;
  Value::NumericKind forced_kind = Value::NumericKind::I64;
};

struct FastNumericWhileGuardIfZeroPlan {
  std::string variable_name;
  I128 replacement = 1;
  bool force_kind = false;
  Value::NumericKind forced_kind = Value::NumericKind::I64;
  const IfStmt* source_stmt = nullptr;
};

struct FastNumericWhileBlockPlan {
  std::string index_name;
  long long index_step = 1;
  bool limit_is_variable = false;
  long long limit_value = 0;
  std::string limit_variable;
  std::vector<FastNumericWhilePlan> assign_plans;
  std::vector<FastNumericWhileIntExprAssignPlan> int_expr_assign_plans;
  std::vector<FastNumericWhileGuardIfZeroPlan> guard_plans;
  std::vector<FastNumericWhileBlockStep> steps;
};

bool parse_fast_int_expr_assign_stmt(const AssignStmt& assign,
                                     const std::string& index_name,
                                     FastNumericWhileIntExprAssignPlan& out) {
  const auto* target = as_variable_expr(assign.target.get());
  if (!target || target->name == index_name || !assign.value) {
    return false;
  }

  out.target_name = target->name;
  out.source_stmt = &assign;
  out.expr = assign.value.get();
  out.force_kind = false;
  out.forced_kind = Value::NumericKind::I64;

  Value::NumericKind ctor_kind = Value::NumericKind::I64;
  const Expr* ctor_arg = nullptr;
  if (parse_int_numeric_constructor_call(assign.value.get(), ctor_kind, ctor_arg)) {
    out.force_kind = true;
    out.forced_kind = ctor_kind;
    out.expr = ctor_arg;
  }

  if (!expr_supports_fast_int_eval_while(out.expr)) {
    return false;
  }
  // `/` and `^` promote int-int to floating semantics in the generic evaluator.
  // Keep strict behavior: only use this int fast-path when assignment explicitly
  // constrains the result back into an integer constructor.
  if (!out.force_kind && expr_has_float_promoting_ops_while(out.expr)) {
    return false;
  }
  return true;
}

bool parse_guard_if_zero_stmt(const Stmt* stmt,
                              FastNumericWhileGuardIfZeroPlan& out) {
  const auto* if_stmt = as_if_stmt(stmt);
  if (!if_stmt || !if_stmt->else_body.empty() || !if_stmt->elif_branches.empty() ||
      if_stmt->then_body.size() != 1) {
    return false;
  }

  const auto* cond = as_binary_expr(if_stmt->condition.get());
  if (!cond || cond->op != BinaryOp::Eq || !cond->left || !cond->right) {
    return false;
  }

  const auto parse_zero_expr = [](const Expr* expr,
                                  bool& has_kind,
                                  Value::NumericKind& kind) -> bool {
    I128 value = 0;
    if (parse_i128_literal_expr_while(expr, value)) {
      has_kind = false;
      return value == 0;
    }
    Value::NumericKind ctor_kind = Value::NumericKind::I64;
    const Expr* ctor_arg = nullptr;
    if (!parse_int_numeric_constructor_call(expr, ctor_kind, ctor_arg)) {
      return false;
    }
    if (!parse_i128_literal_expr_while(ctor_arg, value)) {
      return false;
    }
    has_kind = true;
    kind = ctor_kind;
    return value == 0;
  };

  const auto* lhs_var = as_variable_expr(cond->left.get());
  const auto* rhs_var = as_variable_expr(cond->right.get());
  std::string guarded_name;
  bool cond_has_kind = false;
  Value::NumericKind cond_kind = Value::NumericKind::I64;
  if (lhs_var) {
    if (!parse_zero_expr(cond->right.get(), cond_has_kind, cond_kind)) {
      return false;
    }
    guarded_name = lhs_var->name;
  } else if (rhs_var) {
    if (!parse_zero_expr(cond->left.get(), cond_has_kind, cond_kind)) {
      return false;
    }
    guarded_name = rhs_var->name;
  } else {
    return false;
  }

  const auto* then_assign = as_assign_stmt(if_stmt->then_body[0].get());
  if (!then_assign) {
    return false;
  }
  const auto* then_target = as_variable_expr(then_assign->target.get());
  if (!then_target || then_target->name != guarded_name || !then_assign->value) {
    return false;
  }

  I128 replacement = 0;
  bool assign_has_kind = false;
  Value::NumericKind assign_kind = Value::NumericKind::I64;
  Value::NumericKind ctor_kind = Value::NumericKind::I64;
  const Expr* ctor_arg = nullptr;
  if (parse_int_numeric_constructor_call(then_assign->value.get(), ctor_kind, ctor_arg)) {
    if (!parse_i128_literal_expr_while(ctor_arg, replacement)) {
      return false;
    }
    assign_has_kind = true;
    assign_kind = ctor_kind;
  } else {
    if (!parse_i128_literal_expr_while(then_assign->value.get(), replacement)) {
      return false;
    }
  }

  out.variable_name = guarded_name;
  out.replacement = replacement;
  out.force_kind = assign_has_kind ? true : cond_has_kind;
  out.forced_kind = assign_has_kind ? assign_kind : cond_kind;
  out.source_stmt = if_stmt;
  return true;
}

std::optional<FastNumericWhileBlockPlan> build_fast_numeric_while_block_plan(
    const WhileStmt& while_stmt) {
  if (while_stmt.body.size() < 3) {
    return std::nullopt;
  }

  const auto* condition = as_binary_expr(while_stmt.condition.get());
  if (!condition || condition->op != BinaryOp::Lt) {
    return std::nullopt;
  }
  const auto* index_var = as_variable_expr(condition->left.get());
  if (!index_var) {
    return std::nullopt;
  }

  FastNumericWhileBlockPlan plan;
  plan.index_name = index_var->name;
  if (const auto* limit_number = as_number_expr(condition->right.get())) {
    const auto maybe_limit = number_to_i64(*limit_number);
    if (!maybe_limit.has_value()) {
      return std::nullopt;
    }
    plan.limit_value = *maybe_limit;
  } else if (const auto* limit_variable = as_variable_expr(condition->right.get())) {
    plan.limit_is_variable = true;
    plan.limit_variable = limit_variable->name;
  } else {
    return std::nullopt;
  }

  bool seen_increment = false;
  for (const auto& stmt : while_stmt.body) {
    const auto* assign = as_assign_stmt(stmt.get());
    if (assign) {
      if (const auto step = parse_index_step_assign(*assign, plan.index_name); step.has_value()) {
        if (seen_increment || *step <= 0) {
          return std::nullopt;
        }
        plan.index_step = *step;
        seen_increment = true;
        plan.steps.push_back(FastNumericWhileBlockStep{
            .kind = FastNumericWhileBlockStep::Kind::Increment,
            .step_index = 0,
        });
        continue;
      }

      FastNumericWhilePlan assign_plan;
      if (parse_fast_numeric_assign(*assign, plan.index_name, assign_plan)) {
        if (assign_plan.accumulator_name == plan.index_name) {
          return std::nullopt;
        }
        const std::size_t assign_index = plan.assign_plans.size();
        plan.assign_plans.push_back(std::move(assign_plan));
        plan.steps.push_back(FastNumericWhileBlockStep{
            .kind = FastNumericWhileBlockStep::Kind::Assign,
            .step_index = assign_index,
        });
        continue;
      }

      FastNumericWhileIntExprAssignPlan int_expr_plan;
      if (!parse_fast_int_expr_assign_stmt(*assign, plan.index_name, int_expr_plan)) {
        return std::nullopt;
      }
      if (int_expr_plan.target_name == plan.index_name) {
        return std::nullopt;
      }
      const std::size_t int_expr_index = plan.int_expr_assign_plans.size();
      plan.int_expr_assign_plans.push_back(std::move(int_expr_plan));
      plan.steps.push_back(FastNumericWhileBlockStep{
          .kind = FastNumericWhileBlockStep::Kind::IntExprAssign,
          .step_index = int_expr_index,
      });
      continue;
    }

    FastNumericWhileGuardIfZeroPlan guard_plan;
    if (parse_guard_if_zero_stmt(stmt.get(), guard_plan)) {
      if (guard_plan.variable_name == plan.index_name) {
        return std::nullopt;
      }
      const std::size_t guard_index = plan.guard_plans.size();
      plan.guard_plans.push_back(std::move(guard_plan));
      plan.steps.push_back(FastNumericWhileBlockStep{
          .kind = FastNumericWhileBlockStep::Kind::GuardIfZero,
          .step_index = guard_index,
      });
      continue;
    }
    return std::nullopt;
  }

  if (!seen_increment ||
      (plan.assign_plans.empty() && plan.int_expr_assign_plans.empty())) {
    return std::nullopt;
  }

  if (plan.limit_is_variable) {
    if (plan.limit_variable == plan.index_name) {
      return std::nullopt;
    }
    for (const auto& assign_plan : plan.assign_plans) {
      if (assign_plan.accumulator_name == plan.limit_variable) {
        return std::nullopt;
      }
    }
    for (const auto& assign_plan : plan.int_expr_assign_plans) {
      if (assign_plan.target_name == plan.limit_variable) {
        return std::nullopt;
      }
    }
    for (const auto& guard_plan : plan.guard_plans) {
      if (guard_plan.variable_name == plan.limit_variable) {
        return std::nullopt;
      }
    }
  }

  return plan;
}

bool resolve_loop_limit(const FastNumericWhileBlockPlan& plan,
                        const std::shared_ptr<Environment>& env, long long& out_limit) {
  if (!plan.limit_is_variable) {
    out_limit = plan.limit_value;
    return true;
  }
  const auto* value = env->get_ptr(plan.limit_variable);
  if (!value || !is_numeric_kind(*value)) {
    return false;
  }
  out_limit = value_to_int(*value);
  return true;
}

void increment_index_checked(Value& index) {
  I128 current = 0;
  if (!try_numeric_like_to_i128_while(index, current)) {
    throw EvalException("fast while path expects integer loop index");
  }
  I128 next = 0;
  if (__builtin_add_overflow(current, static_cast<I128>(1), &next)) {
    throw EvalException("integer overflow in while loop index increment");
  }
  if (!assign_i128_to_fast_target_while(index, next, false, Value::NumericKind::I64)) {
    throw EvalException("fast while path failed to assign integer loop index");
  }
}

void increment_index_checked(Value& index, long long step) {
  I128 current = 0;
  if (!try_numeric_like_to_i128_while(index, current)) {
    throw EvalException("fast while path expects integer loop index");
  }
  I128 next = 0;
  if (__builtin_add_overflow(current, static_cast<I128>(step), &next)) {
    throw EvalException("integer overflow in while loop index increment");
  }
  if (!assign_i128_to_fast_target_while(index, next, false, Value::NumericKind::I64)) {
    throw EvalException("fast while path failed to assign integer loop index");
  }
}

struct FastNumericWhileOperandRef {
  bool is_variable = false;
  bool dynamic_lookup = false;
  std::string variable;
  const Value* stable_ptr = nullptr;
  Value constant = Value::nil();
};

struct FastNumericWhileAssignRuntime {
  Value* target = nullptr;
  BinaryOp op = BinaryOp::Add;
  bool lhs_targets_accumulator = false;
  bool rhs_targets_accumulator = false;
  FastNumericWhileOperandRef lhs;
  FastNumericWhileOperandRef rhs;
};

struct FastNumericWhileIntExprAssignRuntime {
  Value* target = nullptr;
  std::string target_name;
  int local_slot = -1;
  const AssignStmt* source_stmt = nullptr;
  const Expr* expr = nullptr;
  bool force_kind = false;
  Value::NumericKind forced_kind = Value::NumericKind::I64;
  struct FastIntExprInstruction {
    enum class Op {
      PushConst,
      PushVar,
      AffineMod,
      ModSub,
      Neg,
      Add,
      Sub,
      Mul,
      Div,
      Mod,
      Pow,
    };
    Op op = Op::PushConst;
    I128 imm = 0;
    I128 imm2 = 0;
    I128 imm3 = 0;
    const Value* value_ptr = nullptr;
    std::string variable_name;
    bool dynamic_lookup = false;
    int local_slot = -1;
  };
  std::vector<FastIntExprInstruction> code;
  std::vector<I128> stack;
};

struct FastNumericWhileGuardRuntime {
  Value* target = nullptr;
  std::string target_name;
  int local_slot = -1;
  const IfStmt* source_stmt = nullptr;
  I128 replacement = 1;
  bool force_kind = false;
  Value::NumericKind forced_kind = Value::NumericKind::I64;
};

bool try_numeric_like_to_i128_while(const Value& value, I128& out) {
  if (value.kind == Value::Kind::Int) {
    out = static_cast<I128>(value.int_value);
    return true;
  }
  if (value.kind == Value::Kind::Numeric && value.numeric_value &&
      numeric_kind_is_int(value.numeric_value->kind) &&
      value.numeric_value->parsed_int_valid) {
    out = value.numeric_value->parsed_int;
    return true;
  }
  return false;
}

bool try_pow_i128_checked_while(I128 base, I128 exponent, I128& out) {
  if (exponent < 0) {
    return false;
  }
  using U128Local = unsigned __int128;
  I128 acc = 1;
  I128 factor = base;
  U128Local e = static_cast<U128Local>(exponent);
  while (e != 0) {
    if ((e & 1U) != 0U) {
      if (__builtin_mul_overflow(acc, factor, &acc)) {
        return false;
      }
    }
    e >>= 1U;
    if (e != 0U && __builtin_mul_overflow(factor, factor, &factor)) {
      return false;
    }
  }
  out = acc;
  return true;
}

bool compile_fast_int_expr_while(
    const Expr* expr,
    const std::shared_ptr<Environment>& env,
    std::vector<FastNumericWhileIntExprAssignRuntime::FastIntExprInstruction>& out_code) {
  using Instr = FastNumericWhileIntExprAssignRuntime::FastIntExprInstruction;
  const auto resolve_var_ptr =
      [&](const Expr* node, const Value*& out_ptr, std::string& out_name, bool& out_dynamic) -> bool {
    const auto* variable = as_variable_expr(node);
    if (!variable) {
      return false;
    }
    out_name = variable->name;
    out_ptr = env ? env->get_ptr(variable->name) : nullptr;
    out_dynamic = (out_ptr == nullptr);
    return true;
  };
  const auto parse_const_i128 = [&](const Expr* node, I128& out) -> bool {
    return parse_i128_literal_expr_while(node, out);
  };
  const auto parse_mul_var_const =
      [&](const Expr* node, const Value*& out_var, std::string& out_name,
          bool& out_dynamic, I128& out_const) -> bool {
    const auto* mul = as_binary_expr(node);
    if (!mul || mul->op != BinaryOp::Mul) {
      return false;
    }
    if (resolve_var_ptr(mul->left.get(), out_var, out_name, out_dynamic) &&
        parse_const_i128(mul->right.get(), out_const)) {
      return true;
    }
    if (resolve_var_ptr(mul->right.get(), out_var, out_name, out_dynamic) &&
        parse_const_i128(mul->left.get(), out_const)) {
      return true;
    }
    return false;
  };
  const auto try_compile_affine_mod = [&](const Expr* node, Instr& out) -> bool {
    const auto* mod = as_binary_expr(node);
    if (!mod || mod->op != BinaryOp::Mod) {
      return false;
    }
    I128 modulus = 0;
    if (!parse_const_i128(mod->right.get(), modulus) || modulus == 0) {
      return false;
    }
    const auto* add = as_binary_expr(mod->left.get());
    if (!add || add->op != BinaryOp::Add) {
      return false;
    }
    const Value* var_ptr = nullptr;
    std::string var_name;
    bool var_dynamic = false;
    I128 mul_const = 0;
    I128 add_const = 0;
    if (parse_mul_var_const(add->left.get(), var_ptr, var_name, var_dynamic, mul_const) &&
        parse_const_i128(add->right.get(), add_const)) {
      out = Instr{
          .op = Instr::Op::AffineMod,
          .imm = mul_const,
          .imm2 = add_const,
          .imm3 = modulus,
          .value_ptr = var_ptr,
          .variable_name = var_name,
          .dynamic_lookup = var_dynamic,
      };
      return true;
    }
    if (parse_mul_var_const(add->right.get(), var_ptr, var_name, var_dynamic, mul_const) &&
        parse_const_i128(add->left.get(), add_const)) {
      out = Instr{
          .op = Instr::Op::AffineMod,
          .imm = mul_const,
          .imm2 = add_const,
          .imm3 = modulus,
          .value_ptr = var_ptr,
          .variable_name = var_name,
          .dynamic_lookup = var_dynamic,
      };
      return true;
    }
    return false;
  };
  const auto try_compile_mod_sub = [&](const Expr* node, Instr& out) -> bool {
    const auto* sub = as_binary_expr(node);
    if (!sub || sub->op != BinaryOp::Sub) {
      return false;
    }
    I128 bias = 0;
    if (!parse_const_i128(sub->right.get(), bias)) {
      return false;
    }
    const auto* mod = as_binary_expr(sub->left.get());
    if (!mod || mod->op != BinaryOp::Mod) {
      return false;
    }
    const Value* var_ptr = nullptr;
    std::string var_name;
    bool var_dynamic = false;
    I128 modulus = 0;
    if (!resolve_var_ptr(mod->left.get(), var_ptr, var_name, var_dynamic) ||
        !parse_const_i128(mod->right.get(), modulus) ||
        modulus == 0) {
      return false;
    }
    out = Instr{
        .op = Instr::Op::ModSub,
        .imm = modulus,
        .imm2 = bias,
        .value_ptr = var_ptr,
        .variable_name = var_name,
        .dynamic_lookup = var_dynamic,
    };
    return true;
  };

  if (!expr) {
    return false;
  }
  {
    Instr fast{};
    if (try_compile_affine_mod(expr, fast) || try_compile_mod_sub(expr, fast)) {
      out_code.push_back(fast);
      return true;
    }
  }
  if (const auto* number = as_number_expr(expr)) {
    const auto maybe = number_to_i64(*number);
    if (!maybe.has_value()) {
      return false;
    }
    out_code.push_back(Instr{
        .op = Instr::Op::PushConst,
        .imm = static_cast<I128>(*maybe),
    });
    return true;
  }
  if (const auto* variable = as_variable_expr(expr)) {
    const auto* ptr = env ? env->get_ptr(variable->name) : nullptr;
    out_code.push_back(Instr{
        .op = Instr::Op::PushVar,
        .imm = 0,
        .value_ptr = ptr,
        .variable_name = variable->name,
        .dynamic_lookup = (ptr == nullptr),
    });
    return true;
  }
  if (const auto* unary = as_unary_expr(expr)) {
    if (unary->op != UnaryOp::Neg || !compile_fast_int_expr_while(unary->operand.get(), env, out_code)) {
      return false;
    }
    out_code.push_back(Instr{.op = Instr::Op::Neg});
    return true;
  }
  if (const auto* binary = as_binary_expr(expr)) {
    if (!compile_fast_int_expr_while(binary->left.get(), env, out_code) ||
        !compile_fast_int_expr_while(binary->right.get(), env, out_code)) {
      return false;
    }
    Instr::Op op = Instr::Op::Add;
    switch (binary->op) {
      case BinaryOp::Add:
        op = Instr::Op::Add;
        break;
      case BinaryOp::Sub:
        op = Instr::Op::Sub;
        break;
      case BinaryOp::Mul:
        op = Instr::Op::Mul;
        break;
      case BinaryOp::Div:
        op = Instr::Op::Div;
        break;
      case BinaryOp::Mod:
        op = Instr::Op::Mod;
        break;
      case BinaryOp::Pow:
        op = Instr::Op::Pow;
        break;
      default:
        return false;
    }
    out_code.push_back(Instr{.op = op});
    return true;
  }
  Value::NumericKind ctor_kind = Value::NumericKind::I64;
  const Expr* ctor_arg = nullptr;
  if (parse_int_numeric_constructor_call(expr, ctor_kind, ctor_arg)) {
    (void)ctor_kind;
    return compile_fast_int_expr_while(ctor_arg, env, out_code);
  }
  return false;
}

bool eval_compiled_fast_int_expr_while(
    std::vector<FastNumericWhileIntExprAssignRuntime::FastIntExprInstruction>& code,
    const std::shared_ptr<Environment>& env,
    std::vector<I128>& stack,
    I128& out) {
  stack.clear();
  for (auto& instr : code) {
    using Op = FastNumericWhileIntExprAssignRuntime::FastIntExprInstruction::Op;
    switch (instr.op) {
      case Op::PushConst:
        stack.push_back(instr.imm);
        break;
      case Op::PushVar: {
        const Value* value_ptr = instr.value_ptr;
        if (instr.dynamic_lookup || !value_ptr) {
          value_ptr = env ? env->get_ptr(instr.variable_name) : nullptr;
          if (!value_ptr) {
            return false;
          }
          instr.value_ptr = value_ptr;
          instr.dynamic_lookup = false;
        }
        if (!value_ptr) {
          return false;
        }
        I128 value = 0;
        if (!try_numeric_like_to_i128_while(*value_ptr, value)) {
          return false;
        }
        stack.push_back(value);
        break;
      }
      case Op::AffineMod: {
        const Value* value_ptr = instr.value_ptr;
        if ((instr.dynamic_lookup || !value_ptr) && !instr.variable_name.empty()) {
          value_ptr = env ? env->get_ptr(instr.variable_name) : nullptr;
          if (!value_ptr) {
            return false;
          }
          instr.value_ptr = value_ptr;
          instr.dynamic_lookup = false;
        }
        if (!value_ptr || instr.imm3 == 0) {
          return false;
        }
        I128 base = 0;
        if (!try_numeric_like_to_i128_while(*value_ptr, base)) {
          return false;
        }
        I128 scaled = 0;
        I128 shifted = 0;
        if (__builtin_mul_overflow(base, instr.imm, &scaled) ||
            __builtin_add_overflow(scaled, instr.imm2, &shifted)) {
          return false;
        }
        I128 reduced = 0;
        if (!try_fast_i128_mod_pow2_nonneg(shifted, instr.imm3, reduced)) {
          reduced = shifted % instr.imm3;
        }
        stack.push_back(reduced);
        break;
      }
      case Op::ModSub: {
        const Value* value_ptr = instr.value_ptr;
        if ((instr.dynamic_lookup || !value_ptr) && !instr.variable_name.empty()) {
          value_ptr = env ? env->get_ptr(instr.variable_name) : nullptr;
          if (!value_ptr) {
            return false;
          }
          instr.value_ptr = value_ptr;
          instr.dynamic_lookup = false;
        }
        if (!value_ptr || instr.imm == 0) {
          return false;
        }
        I128 base = 0;
        if (!try_numeric_like_to_i128_while(*value_ptr, base)) {
          return false;
        }
        I128 reduced = 0;
        if (!try_fast_i128_mod_pow2_nonneg(base, instr.imm, reduced)) {
          reduced = base % instr.imm;
        }
        I128 shifted = 0;
        if (__builtin_sub_overflow(reduced, instr.imm2, &shifted)) {
          return false;
        }
        stack.push_back(shifted);
        break;
      }
      case Op::Neg:
        if (stack.empty()) {
          return false;
        }
        stack.back() = -stack.back();
        break;
      case Op::Add:
      case Op::Sub:
      case Op::Mul:
      case Op::Div:
      case Op::Mod:
      case Op::Pow: {
        if (stack.size() < 2) {
          return false;
        }
        const I128 rhs = stack.back();
        stack.pop_back();
        const I128 lhs = stack.back();
        stack.pop_back();
        I128 value = 0;
        if (instr.op == Op::Add) {
          if (__builtin_add_overflow(lhs, rhs, &value)) {
            return false;
          }
        } else if (instr.op == Op::Sub) {
          if (__builtin_sub_overflow(lhs, rhs, &value)) {
            return false;
          }
        } else if (instr.op == Op::Mul) {
          if (__builtin_mul_overflow(lhs, rhs, &value)) {
            return false;
          }
        } else if (instr.op == Op::Div) {
          if (rhs == 0) {
            throw EvalException("division by zero");
          }
          value = lhs / rhs;
        } else if (instr.op == Op::Mod) {
          if (rhs == 0) {
            throw EvalException("modulo by zero");
          }
          if (!try_fast_i128_mod_pow2_nonneg(lhs, rhs, value)) {
            value = lhs % rhs;
          }
        } else {
          if (!try_pow_i128_checked_while(lhs, rhs, value)) {
            return false;
          }
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

bool try_eval_fast_int_expr_while(const Expr* expr,
                                  const std::shared_ptr<Environment>& env,
                                  I128& out) {
  if (!expr) {
    return false;
  }
  if (const auto* number = as_number_expr(expr)) {
    const auto maybe = number_to_i64(*number);
    if (!maybe.has_value()) {
      return false;
    }
    out = static_cast<I128>(*maybe);
    return true;
  }
  if (const auto* variable = as_variable_expr(expr)) {
    struct FastIntVarCacheEntry {
      const VariableExpr* expr = nullptr;
      std::uint64_t env_id = 0;
      std::size_t values_size = 0;
      std::size_t bucket_count = 0;
      const Value* value = nullptr;
    };
    constexpr std::size_t kFastIntVarCacheSize = 1024;
    static thread_local std::array<FastIntVarCacheEntry, kFastIntVarCacheSize> cache{};

    const auto key_a = static_cast<std::size_t>(
        reinterpret_cast<std::uintptr_t>(variable));
    const auto key_b =
        static_cast<std::size_t>(env ? env->stable_id : 0) * 11400714819323198485ull;
    auto& slot = cache[(key_a ^ key_b) & (kFastIntVarCacheSize - 1)];
    const auto env_id = env ? env->stable_id : 0;
    const auto values_size = env ? env->values.size() : 0U;
    const auto bucket_count = env ? env->values.bucket_count() : 0U;

    const Value* value = nullptr;
    if (slot.expr == variable && slot.env_id == env_id &&
        slot.values_size == values_size && slot.bucket_count == bucket_count &&
        slot.value != nullptr) {
      value = slot.value;
    } else {
      value = env ? env->get_ptr(variable->name) : nullptr;
      slot.expr = variable;
      slot.env_id = env_id;
      slot.values_size = values_size;
      slot.bucket_count = bucket_count;
      slot.value = value;
    }
    if (!value) {
      return false;
    }
    return try_numeric_like_to_i128_while(*value, out);
  }
  if (const auto* unary = as_unary_expr(expr)) {
    if (unary->op != UnaryOp::Neg || !unary->operand) {
      return false;
    }
    I128 operand = 0;
    if (!try_eval_fast_int_expr_while(unary->operand.get(), env, operand)) {
      return false;
    }
    out = -operand;
    return true;
  }
  if (const auto* binary = as_binary_expr(expr)) {
    I128 lhs = 0;
    I128 rhs = 0;
    if (!try_eval_fast_int_expr_while(binary->left.get(), env, lhs) ||
        !try_eval_fast_int_expr_while(binary->right.get(), env, rhs)) {
      return false;
    }
    switch (binary->op) {
      case BinaryOp::Add:
        if (__builtin_add_overflow(lhs, rhs, &out)) {
          return false;
        }
        return true;
      case BinaryOp::Sub:
        if (__builtin_sub_overflow(lhs, rhs, &out)) {
          return false;
        }
        return true;
      case BinaryOp::Mul:
        if (__builtin_mul_overflow(lhs, rhs, &out)) {
          return false;
        }
        return true;
      case BinaryOp::Div:
        if (rhs == 0) {
          throw EvalException("division by zero");
        }
        out = lhs / rhs;
        return true;
      case BinaryOp::Mod:
        if (rhs == 0) {
          throw EvalException("modulo by zero");
        }
        if (try_fast_i128_mod_pow2_nonneg(lhs, rhs, out)) {
          return true;
        }
        out = lhs % rhs;
        return true;
      case BinaryOp::Pow:
        return try_pow_i128_checked_while(lhs, rhs, out);
      default:
        return false;
    }
  }
  Value::NumericKind ctor_kind = Value::NumericKind::I64;
  const Expr* ctor_arg = nullptr;
  if (parse_int_numeric_constructor_call(expr, ctor_kind, ctor_arg)) {
    (void)ctor_kind;
    return try_eval_fast_int_expr_while(ctor_arg, env, out);
  }
  return false;
}

bool assign_i128_to_fast_target_while(Value& target, I128 value,
                                      bool force_kind,
                                      Value::NumericKind forced_kind) {
  if (force_kind) {
    assign_numeric_int_value_inplace(target, forced_kind, value);
    return true;
  }
  if (target.kind == Value::Kind::Int) {
    if (value < static_cast<I128>(std::numeric_limits<long long>::min()) ||
        value > static_cast<I128>(std::numeric_limits<long long>::max())) {
      return false;
    }
    target.int_value = static_cast<long long>(value);
    return true;
  }
  if (target.kind == Value::Kind::Numeric && target.numeric_value &&
      numeric_kind_is_int(target.numeric_value->kind)) {
    assign_numeric_int_value_inplace(target, target.numeric_value->kind, value);
    return true;
  }
  return false;
}

bool apply_integer_alias_op_raw(BinaryOp op, I128 lhs, I128 rhs_v, int bits, I128& out) {
  out = lhs;
  switch (op) {
    case BinaryOp::Add: {
      if (__builtin_add_overflow(lhs, rhs_v, &out)) {
        out = (lhs >= 0 && rhs_v >= 0) ? i128_max() : i128_min();
      }
      out = clamp_to_signed_bits(out, bits);
      return true;
    }
    case BinaryOp::Sub: {
      if (__builtin_sub_overflow(lhs, rhs_v, &out)) {
        out = (lhs >= 0 && rhs_v < 0) ? i128_max() : i128_min();
      }
      out = clamp_to_signed_bits(out, bits);
      return true;
    }
    case BinaryOp::Mul: {
      if (__builtin_mul_overflow(lhs, rhs_v, &out)) {
        const bool non_negative = (lhs == 0 || rhs_v == 0) || ((lhs > 0) == (rhs_v > 0));
        out = non_negative ? i128_max() : i128_min();
      }
      out = clamp_to_signed_bits(out, bits);
      return true;
    }
    case BinaryOp::Mod:
      if (rhs_v == 0) {
        throw EvalException("modulo by zero");
      }
      out = clamp_to_signed_bits(lhs % rhs_v, bits);
      return true;
    default:
      return false;
  }
}

bool try_apply_integer_alias_op_fast(BinaryOp op, Value& target, const Value& rhs) {
  if (target.kind != Value::Kind::Numeric || rhs.kind != Value::Kind::Numeric ||
      !target.numeric_value || !rhs.numeric_value) {
    return false;
  }

  const auto kind = target.numeric_value->kind;
  if (!numeric_kind_is_int(kind) || is_extended_int_kind_local(kind) ||
      rhs.numeric_value->kind != kind) {
    return false;
  }

  if (op != BinaryOp::Add && op != BinaryOp::Sub &&
      op != BinaryOp::Mul && op != BinaryOp::Mod) {
    return false;
  }

  const int bits = effective_int_bits(kind);
  const auto lhs = target.numeric_value->parsed_int_valid
                       ? clamp_to_signed_bits(target.numeric_value->parsed_int, bits)
                       : clamp_to_signed_bits(value_to_i128(target), bits);
  const auto rhs_v = rhs.numeric_value->parsed_int_valid
                         ? clamp_to_signed_bits(rhs.numeric_value->parsed_int, bits)
                         : clamp_to_signed_bits(value_to_i128(rhs), bits);

  I128 out = lhs;
  if (!apply_integer_alias_op_raw(op, lhs, rhs_v, bits, out)) {
    return false;
  }
  assign_numeric_int_value_inplace(target, kind, out);
  return true;
}

bool initialize_fast_numeric_operand_ref(FastNumericWhileOperandRef& out,
                                         bool is_variable, const std::string& variable,
                                         const Expr* literal_expr,
                                         const std::unordered_set<std::string>& dynamic_names,
                                         Interpreter& self,
                                         const std::shared_ptr<Environment>& env) {
  out.is_variable = is_variable;
  if (!is_variable) {
    if (!literal_expr) {
      return false;
    }
    out.constant = self.evaluate(*literal_expr, env);
    return is_numeric_kind(out.constant);
  }

  out.variable = variable;
  out.dynamic_lookup = dynamic_names.find(variable) != dynamic_names.end();
  if (out.dynamic_lookup) {
    return true;
  }
  out.stable_ptr = env->get_ptr(variable);
  return out.stable_ptr != nullptr && is_numeric_kind(*out.stable_ptr);
}

const Value* resolve_fast_numeric_operand_ref(const FastNumericWhileOperandRef& operand,
                                              const std::shared_ptr<Environment>& env) {
  if (!operand.is_variable) {
    return &operand.constant;
  }
  if (!operand.dynamic_lookup) {
    return operand.stable_ptr;
  }
  return env->get_ptr(operand.variable);
}

bool try_execute_fast_numeric_while_block_int_fused(const FastNumericWhileBlockPlan& plan,
                                                    std::vector<FastNumericWhileAssignRuntime>& runtime_assigns,
                                                    Value& index,
                                                    const std::shared_ptr<Environment>& env,
                                                    Value& result) {
  if (runtime_assigns.empty()) {
    return false;
  }
  if (index.kind != Value::Kind::Int) {
    return false;
  }

  auto* target = runtime_assigns.front().target;
  if (!target || target->kind != Value::Kind::Numeric || !target->numeric_value) {
    return false;
  }
  const auto kind = target->numeric_value->kind;
  if (!numeric_kind_is_int(kind) || is_extended_int_kind_local(kind)) {
    return false;
  }
  const int bits = effective_int_bits(kind);

  struct FusedAssignSpec {
    BinaryOp op = BinaryOp::Add;
    I128 rhs = 0;
  };
  std::vector<FusedAssignSpec> assign_specs(runtime_assigns.size());
  std::vector<bool> assign_spec_ready(runtime_assigns.size(), false);

  for (std::size_t i = 0; i < runtime_assigns.size(); ++i) {
    auto& runtime = runtime_assigns[i];
    if (!runtime.target || runtime.target != target || !runtime.lhs_targets_accumulator ||
        runtime.rhs_targets_accumulator) {
      return false;
    }
    // The fused path requires rhs invariance across iterations.
    if (runtime.rhs.dynamic_lookup) {
      return false;
    }
    const auto* rhs_ptr = resolve_fast_numeric_operand_ref(runtime.rhs, env);
    if (!rhs_ptr || rhs_ptr->kind != Value::Kind::Numeric || !rhs_ptr->numeric_value ||
        rhs_ptr->numeric_value->kind != kind) {
      return false;
    }
    if (runtime.op != BinaryOp::Add && runtime.op != BinaryOp::Sub &&
        runtime.op != BinaryOp::Mul && runtime.op != BinaryOp::Mod) {
      return false;
    }

    assign_specs[i].op = runtime.op;
    assign_specs[i].rhs = rhs_ptr->numeric_value->parsed_int_valid
                              ? clamp_to_signed_bits(rhs_ptr->numeric_value->parsed_int, bits)
                              : clamp_to_signed_bits(value_to_i128(*rhs_ptr), bits);
    assign_spec_ready[i] = true;
  }

  I128 acc = target->numeric_value->parsed_int_valid
                 ? clamp_to_signed_bits(target->numeric_value->parsed_int, bits)
                 : clamp_to_signed_bits(value_to_i128(*target), bits);
  result = Value::nil();

  while (true) {
    long long limit = 0;
    if (!resolve_loop_limit(plan, env, limit)) {
      return false;
    }
    if (index.int_value >= limit) {
      break;
    }

    for (const auto& step : plan.steps) {
      if (step.kind == FastNumericWhileBlockStep::Kind::Increment) {
        increment_index_checked(index, plan.index_step);
        continue;
      }
      if (step.kind != FastNumericWhileBlockStep::Kind::Assign) {
        return false;
      }

      if (step.step_index >= assign_specs.size() || !assign_spec_ready[step.step_index]) {
        return false;
      }
      const auto& assign = assign_specs[step.step_index];
      I128 out = acc;
      if (!apply_integer_alias_op_raw(assign.op, acc, assign.rhs, bits, out)) {
        return false;
      }
      acc = out;
    }
  }

  assign_numeric_int_value_inplace(*target, kind, acc);
  result = *target;
  return true;
}

bool try_execute_fast_numeric_while_block(const WhileStmt& while_stmt, Interpreter& self,
                                          const std::shared_ptr<Environment>& env,
                                          Value& result) {
  constexpr bool trace_fast_while = false;
  const auto fail = [&](const char* reason) -> bool {
    (void)reason;
    if (trace_fast_while) {
    }
    return false;
  };

  const auto plan = build_fast_numeric_while_block_plan(while_stmt);
  if (!plan.has_value()) {
    return fail("plan:none");
  }

  auto* index_ptr = env->get_ptr(plan->index_name);
  if (!index_ptr || index_ptr->kind != Value::Kind::Int) {
    return fail("index:not-int");
  }

  std::unordered_set<std::string> dynamic_names;
  dynamic_names.reserve(plan->assign_plans.size() + 1);
  dynamic_names.insert(plan->index_name);
  for (const auto& assign_plan : plan->assign_plans) {
    dynamic_names.insert(assign_plan.accumulator_name);
  }

  std::vector<FastNumericWhileAssignRuntime> runtime_assigns;
  runtime_assigns.reserve(plan->assign_plans.size());
  for (const auto& assign_plan : plan->assign_plans) {
    FastNumericWhileAssignRuntime runtime{};
    runtime.op = assign_plan.op;
    runtime.target = env->get_ptr(assign_plan.accumulator_name);
    runtime.lhs_targets_accumulator =
        assign_plan.lhs_is_variable && assign_plan.lhs_variable == assign_plan.accumulator_name;
    runtime.rhs_targets_accumulator =
        assign_plan.rhs_is_variable && assign_plan.rhs_variable == assign_plan.accumulator_name;
    if (!runtime.target || !is_numeric_kind(*runtime.target)) {
      return fail("assign:target-not-numeric");
    }

    if (!initialize_fast_numeric_operand_ref(runtime.lhs, assign_plan.lhs_is_variable,
                                             assign_plan.lhs_variable, assign_plan.lhs_expr,
                                             dynamic_names, self, env) ||
        !initialize_fast_numeric_operand_ref(runtime.rhs, assign_plan.rhs_is_variable,
                                             assign_plan.rhs_variable, assign_plan.rhs_expr,
                                             dynamic_names, self, env)) {
      return fail("assign:operand-init-failed");
    }

    runtime_assigns.push_back(std::move(runtime));
  }

  const auto ensure_intlike_slot = [&](const std::string& name,
                                       bool force_kind,
                                       Value::NumericKind forced_kind) -> Value* {
    if (auto* existing = env->get_ptr(name)) {
      return existing;
    }
    if (force_kind) {
      env->define(name, cast_numeric_to_kind(forced_kind, Value::int_value_of(0)));
    } else {
      env->define(name, Value::int_value_of(0));
    }
    return env->get_ptr(name);
  };

  std::vector<FastNumericWhileIntExprAssignRuntime> runtime_int_expr_assigns;
  runtime_int_expr_assigns.reserve(plan->int_expr_assign_plans.size());
  for (const auto& assign_plan : plan->int_expr_assign_plans) {
    FastNumericWhileIntExprAssignRuntime runtime{};
    runtime.target =
        ensure_intlike_slot(assign_plan.target_name, assign_plan.force_kind, assign_plan.forced_kind);
    if (!runtime.target) {
      return fail("int-expr:target-missing");
    }
    const bool int_target =
        runtime.target->kind == Value::Kind::Int ||
        (runtime.target->kind == Value::Kind::Numeric && runtime.target->numeric_value &&
         numeric_kind_is_int(runtime.target->numeric_value->kind));
    if (!int_target) {
      return fail("int-expr:target-not-intlike");
    }
    runtime.source_stmt = assign_plan.source_stmt;
    runtime.expr = assign_plan.expr;
    runtime.force_kind = assign_plan.force_kind;
    runtime.forced_kind = assign_plan.forced_kind;
    if (!compile_fast_int_expr_while(runtime.expr, env, runtime.code)) {
      return fail("int-expr:compile-failed");
    }
    runtime.stack.reserve(runtime.code.size());
    runtime_int_expr_assigns.push_back(std::move(runtime));
  }

  std::vector<FastNumericWhileGuardRuntime> runtime_guards;
  runtime_guards.reserve(plan->guard_plans.size());
  for (const auto& guard_plan : plan->guard_plans) {
    FastNumericWhileGuardRuntime runtime{};
    runtime.target =
        ensure_intlike_slot(guard_plan.variable_name, guard_plan.force_kind, guard_plan.forced_kind);
    if (!runtime.target) {
      return fail("guard:target-missing");
    }
    const bool int_target =
        runtime.target->kind == Value::Kind::Int ||
        (runtime.target->kind == Value::Kind::Numeric && runtime.target->numeric_value &&
         numeric_kind_is_int(runtime.target->numeric_value->kind));
    if (!int_target) {
      return fail("guard:target-not-intlike");
    }
    runtime.source_stmt = guard_plan.source_stmt;
    runtime.replacement = guard_plan.replacement;
    runtime.force_kind = guard_plan.force_kind;
    runtime.forced_kind = guard_plan.forced_kind;
    runtime_guards.push_back(std::move(runtime));
  }

  if (try_execute_fast_numeric_while_block_int_fused(*plan, runtime_assigns, *index_ptr, env, result)) {
    return true;
  }

  result = Value::nil();
  while (true) {
    long long limit = 0;
    if (!resolve_loop_limit(*plan, env, limit)) {
      return false;
    }
    if (index_ptr->int_value >= limit) {
      break;
    }

    for (const auto& step : plan->steps) {
      if (step.kind == FastNumericWhileBlockStep::Kind::Increment) {
        increment_index_checked(*index_ptr, plan->index_step);
        continue;
      }

      if (step.kind == FastNumericWhileBlockStep::Kind::Assign) {
        if (step.step_index >= runtime_assigns.size()) {
          return false;
        }
        auto& runtime = runtime_assigns[step.step_index];
        const auto* lhs_ptr = runtime.lhs_targets_accumulator
                                  ? runtime.target
                                  : resolve_fast_numeric_operand_ref(runtime.lhs, env);
        const auto* rhs_ptr = runtime.rhs_targets_accumulator
                                  ? runtime.target
                                  : resolve_fast_numeric_operand_ref(runtime.rhs, env);
        if (!lhs_ptr || !rhs_ptr || !is_numeric_kind(*lhs_ptr) || !is_numeric_kind(*rhs_ptr) ||
            !runtime.target || !is_numeric_kind(*runtime.target)) {
          return false;
        }

        if (runtime.lhs_targets_accumulator &&
            try_apply_integer_alias_op_fast(runtime.op, *runtime.target, *rhs_ptr)) {
          result = *runtime.target;
          continue;
        }

        if (!eval_numeric_binary_value_inplace(runtime.op, *lhs_ptr, *rhs_ptr, *runtime.target)) {
          *runtime.target = eval_numeric_binary_value(runtime.op, *lhs_ptr, *rhs_ptr);
        }
        result = *runtime.target;
        continue;
      }

      if (step.kind == FastNumericWhileBlockStep::Kind::IntExprAssign) {
        if (step.step_index >= runtime_int_expr_assigns.size()) {
          return false;
        }
        auto& runtime = runtime_int_expr_assigns[step.step_index];
        auto resolve_push_operand = [&](auto& instr, I128& out_value) -> bool {
          using Op = FastNumericWhileIntExprAssignRuntime::FastIntExprInstruction::Op;
          if (instr.op == Op::PushConst) {
            out_value = instr.imm;
            return true;
          }
          if (instr.op != Op::PushVar) {
            return false;
          }
          const Value* ptr = instr.value_ptr;
          if (instr.dynamic_lookup || !ptr) {
            ptr = env ? env->get_ptr(instr.variable_name) : nullptr;
            if (!ptr) {
              return false;
            }
            instr.value_ptr = ptr;
            instr.dynamic_lookup = false;
          }
          return try_numeric_like_to_i128_while(*ptr, out_value);
        };

        using Op = FastNumericWhileIntExprAssignRuntime::FastIntExprInstruction::Op;
        I128 value = 0;
        if (runtime.code.size() == 1) {
          auto& instr = runtime.code[0];
          if (instr.op == Op::AffineMod || instr.op == Op::ModSub) {
            const Value* ptr = instr.value_ptr;
            if ((instr.dynamic_lookup || !ptr) && !instr.variable_name.empty()) {
              ptr = env ? env->get_ptr(instr.variable_name) : nullptr;
              if (!ptr) {
                return false;
              }
              instr.value_ptr = ptr;
              instr.dynamic_lookup = false;
            }
            if (!ptr) {
              return false;
            }
            I128 base = 0;
            if (!try_numeric_like_to_i128_while(*ptr, base)) {
              return false;
            }
            if (instr.op == Op::AffineMod) {
              if (instr.imm3 == 0) {
                throw EvalException("modulo by zero");
              }
              I128 scaled = 0;
              I128 shifted = 0;
              if (__builtin_mul_overflow(base, instr.imm, &scaled) ||
                  __builtin_add_overflow(scaled, instr.imm2, &shifted)) {
                return false;
              }
              if (!try_fast_i128_mod_pow2_nonneg(shifted, instr.imm3, value)) {
                value = shifted % instr.imm3;
              }
            } else {
              if (instr.imm == 0) {
                throw EvalException("modulo by zero");
              }
              I128 reduced = 0;
              if (!try_fast_i128_mod_pow2_nonneg(base, instr.imm, reduced)) {
                reduced = base % instr.imm;
              }
              if (__builtin_sub_overflow(reduced, instr.imm2, &value)) {
                return false;
              }
            }
            if (assign_i128_to_fast_target_while(*runtime.target, value,
                                                 runtime.force_kind, runtime.forced_kind)) {
              result = *runtime.target;
              continue;
            }
          }
        } else if (runtime.code.size() == 3) {
          auto& lhs_instr = runtime.code[0];
          auto& rhs_instr = runtime.code[1];
          auto& op_instr = runtime.code[2];
          const bool lhs_ok = lhs_instr.op == Op::PushConst || lhs_instr.op == Op::PushVar;
          const bool rhs_ok = rhs_instr.op == Op::PushConst || rhs_instr.op == Op::PushVar;
          if (lhs_ok && rhs_ok &&
              (op_instr.op == Op::Add || op_instr.op == Op::Sub || op_instr.op == Op::Mul ||
               op_instr.op == Op::Div || op_instr.op == Op::Mod || op_instr.op == Op::Pow)) {
            I128 lhs = 0;
            I128 rhs = 0;
            if (resolve_push_operand(lhs_instr, lhs) && resolve_push_operand(rhs_instr, rhs)) {
              bool computed = true;
              switch (op_instr.op) {
                case Op::Add:
                  computed = !__builtin_add_overflow(lhs, rhs, &value);
                  break;
                case Op::Sub:
                  computed = !__builtin_sub_overflow(lhs, rhs, &value);
                  break;
                case Op::Mul:
                  computed = !__builtin_mul_overflow(lhs, rhs, &value);
                  break;
                case Op::Div:
                  if (rhs == 0) {
                    throw EvalException("division by zero");
                  }
                  value = lhs / rhs;
                  break;
                case Op::Mod:
                  if (rhs == 0) {
                    throw EvalException("modulo by zero");
                  }
                  if (!try_fast_i128_mod_pow2_nonneg(lhs, rhs, value)) {
                    value = lhs % rhs;
                  }
                  break;
                case Op::Pow:
                  computed = try_pow_i128_checked_while(lhs, rhs, value);
                  break;
                default:
                  computed = false;
                  break;
              }
              if (computed &&
                  assign_i128_to_fast_target_while(*runtime.target, value,
                                                   runtime.force_kind, runtime.forced_kind)) {
                result = *runtime.target;
                continue;
              }
            }
          }
        }

        if (eval_compiled_fast_int_expr_while(runtime.code, env, runtime.stack, value) &&
            assign_i128_to_fast_target_while(*runtime.target, value,
                                             runtime.force_kind, runtime.forced_kind)) {
          result = *runtime.target;
          continue;
        }
        if (!runtime.source_stmt) {
          return false;
        }
        result = execute_stmt_fast(*runtime.source_stmt, self, env);
        continue;
      }

      if (step.kind == FastNumericWhileBlockStep::Kind::GuardIfZero) {
        if (step.step_index >= runtime_guards.size()) {
          return false;
        }
        auto& runtime = runtime_guards[step.step_index];
        I128 current = 0;
        if (try_numeric_like_to_i128_while(*runtime.target, current)) {
          if (current == 0 &&
              !assign_i128_to_fast_target_while(*runtime.target, runtime.replacement,
                                                runtime.force_kind, runtime.forced_kind)) {
            if (!runtime.source_stmt) {
              return false;
            }
            result = execute_stmt_fast(*runtime.source_stmt, self, env);
            continue;
          }
          result = *runtime.target;
          continue;
        }
        if (!runtime.source_stmt) {
          return false;
        }
        result = execute_stmt_fast(*runtime.source_stmt, self, env);
        continue;
      }

      return false;
    }
  }

  return true;
}

bool try_execute_fast_numeric_while(const WhileStmt& while_stmt, Interpreter& self,
                                    const std::shared_ptr<Environment>& env,
                                    Value& result) {
  const auto plan = build_fast_numeric_while_plan(while_stmt);
  if (!plan.has_value()) {
    return false;
  }

  auto* index_ptr = env->get_ptr(plan->index_name);
  auto* accumulator_ptr = env->get_ptr(plan->accumulator_name);
  if (!index_ptr || !accumulator_ptr || index_ptr->kind != Value::Kind::Int ||
      !is_numeric_kind(*accumulator_ptr)) {
    return false;
  }

  long long limit = 0;
  if (!resolve_loop_limit(*plan, env, limit)) {
    return false;
  }

  Value rhs_constant = Value::nil();
  Value lhs_constant = Value::nil();
  const Value* rhs_ptr = nullptr;
  const Value* lhs_ptr = nullptr;

  if (plan->mode == FastNumericWhileMode::InvariantBinary) {
    if (plan->lhs_is_variable) {
      lhs_ptr = env->get_ptr(plan->lhs_variable);
      if (!lhs_ptr || !is_numeric_kind(*lhs_ptr)) {
        return false;
      }
    } else {
      lhs_constant = self.evaluate(*plan->lhs_expr, env);
      if (!is_numeric_kind(lhs_constant)) {
        return false;
      }
      lhs_ptr = &lhs_constant;
    }
    if (plan->rhs_is_variable) {
      rhs_ptr = env->get_ptr(plan->rhs_variable);
      if (!rhs_ptr || !is_numeric_kind(*rhs_ptr)) {
        return false;
      }
    } else {
      rhs_constant = self.evaluate(*plan->rhs_expr, env);
      if (!is_numeric_kind(rhs_constant)) {
        return false;
      }
      rhs_ptr = &rhs_constant;
    }
  } else {
    if (plan->rhs_is_variable) {
      rhs_ptr = env->get_ptr(plan->rhs_variable);
      if (!rhs_ptr || !is_numeric_kind(*rhs_ptr)) {
        return false;
      }
    } else {
      rhs_constant = self.evaluate(*plan->rhs_expr, env);
      if (!is_numeric_kind(rhs_constant)) {
        return false;
      }
      rhs_ptr = &rhs_constant;
    }
  }

  const bool rhs_depends_on_index =
      plan->rhs_is_variable && plan->rhs_variable == plan->index_name;
  const bool rhs_depends_on_accumulator =
      plan->rhs_is_variable && plan->rhs_variable == plan->accumulator_name;
  const bool lhs_depends_on_index =
      plan->lhs_is_variable && plan->lhs_variable == plan->index_name;
  const bool lhs_depends_on_accumulator =
      plan->lhs_is_variable && plan->lhs_variable == plan->accumulator_name;
  const bool recurrence_repeat_safe =
      plan->mode == FastNumericWhileMode::Recurrence &&
      !rhs_depends_on_index && !rhs_depends_on_accumulator;
  const bool invariant_repeat_safe =
      plan->mode == FastNumericWhileMode::InvariantBinary &&
      !rhs_depends_on_index && !rhs_depends_on_accumulator &&
      !lhs_depends_on_index && !lhs_depends_on_accumulator;

  if (plan->increment_after_operation && (recurrence_repeat_safe || invariant_repeat_safe)) {
    const long long distance = limit - index_ptr->int_value;
    const long long step = plan->index_step;
    const long long remaining = (distance > 0) ? ((distance + step - 1) / step) : 0;
    if (remaining > 0) {
      if (plan->mode == FastNumericWhileMode::InvariantBinary) {
        if (!lhs_ptr || !rhs_ptr || !is_numeric_kind(*lhs_ptr) || !is_numeric_kind(*rhs_ptr)) {
          return false;
        }
        *accumulator_ptr = eval_numeric_binary_value(plan->op, *lhs_ptr, *rhs_ptr);
      } else {
        if (!eval_numeric_repeat_inplace(plan->op, *accumulator_ptr, *rhs_ptr, remaining)) {
          return false;
        }
      }
      const __int128 advanced = static_cast<__int128>(index_ptr->int_value) +
                                static_cast<__int128>(remaining) *
                                    static_cast<__int128>(plan->index_step);
      if (advanced < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
          advanced > static_cast<__int128>(std::numeric_limits<long long>::max())) {
        return false;
      }
      index_ptr->int_value = static_cast<long long>(advanced);
    }
    return true;
  }

  result = Value::nil();
  while (index_ptr->int_value < limit) {
    if (!plan->increment_after_operation) {
      increment_index_checked(*index_ptr, plan->index_step);
    }

    if (plan->mode == FastNumericWhileMode::InvariantBinary) {
      if (plan->lhs_is_variable) {
        lhs_ptr = env->get_ptr(plan->lhs_variable);
        if (!lhs_ptr || !is_numeric_kind(*lhs_ptr)) {
          return false;
        }
      }
      if (plan->rhs_is_variable) {
        rhs_ptr = env->get_ptr(plan->rhs_variable);
        if (!rhs_ptr || !is_numeric_kind(*rhs_ptr)) {
          return false;
        }
      }
      if (!lhs_ptr || !rhs_ptr) {
        return false;
      }
      *accumulator_ptr = eval_numeric_binary_value(plan->op, *lhs_ptr, *rhs_ptr);
    } else {
      if (plan->rhs_is_variable) {
        rhs_ptr = env->get_ptr(plan->rhs_variable);
        if (!rhs_ptr || !is_numeric_kind(*rhs_ptr)) {
          return false;
        }
      }

      if (!eval_numeric_binary_value_inplace(plan->op, *accumulator_ptr, *rhs_ptr,
                                             *accumulator_ptr)) {
        *accumulator_ptr = self.eval_binary(plan->op, *accumulator_ptr, *rhs_ptr);
      }
    }

    if (plan->increment_after_operation) {
      increment_index_checked(*index_ptr, plan->index_step);
    }
  }
  return true;
}

struct FastModuloDispatchPlanWhile {
  std::string selector_variable;
  long long mod_base = 0;
  std::string target_variable;
  std::vector<long long> match_values;
  std::vector<long long> deltas;
  long long else_delta = 0;
  bool has_dense_table = false;
  std::vector<long long> dense_table;
};

bool parse_eq_var_literal_while_dispatch(const Expr* condition, std::string& out_variable,
                                         long long& out_literal) {
  const auto* binary = as_binary_expr(condition);
  if (!binary || binary->op != BinaryOp::Eq) {
    return false;
  }
  if (const auto* lhs_var = as_variable_expr(binary->left.get())) {
    I128 rhs = 0;
    if (!parse_i128_literal_expr_while(binary->right.get(), rhs) ||
        rhs < static_cast<I128>(std::numeric_limits<long long>::min()) ||
        rhs > static_cast<I128>(std::numeric_limits<long long>::max())) {
      return false;
    }
    out_variable = lhs_var->name;
    out_literal = static_cast<long long>(rhs);
    return true;
  }
  if (const auto* rhs_var = as_variable_expr(binary->right.get())) {
    I128 lhs = 0;
    if (!parse_i128_literal_expr_while(binary->left.get(), lhs) ||
        lhs < static_cast<I128>(std::numeric_limits<long long>::min()) ||
        lhs > static_cast<I128>(std::numeric_limits<long long>::max())) {
      return false;
    }
    out_variable = rhs_var->name;
    out_literal = static_cast<long long>(lhs);
    return true;
  }
  return false;
}

std::optional<long long> parse_branch_delta_while_dispatch(const StmtList& body,
                                                           const std::string& target) {
  const auto parse_delta_from_assign = [](const AssignStmt& assign_stmt,
                                          const std::string& assign_target_name) -> std::optional<long long> {
    if (!assign_stmt.target || assign_stmt.target->kind != Expr::Kind::Variable ||
        !assign_stmt.value) {
      return std::nullopt;
    }
    const auto& assign_target = static_cast<const VariableExpr&>(*assign_stmt.target);
    if (assign_target.name != assign_target_name) {
      return std::nullopt;
    }
    const auto* binary = as_binary_expr(assign_stmt.value.get());
    if (!binary) {
      return std::nullopt;
    }
    const auto* lhs_var = as_variable_expr(binary->left.get());
    const auto* rhs_var = as_variable_expr(binary->right.get());
    I128 lhs_lit_i128 = 0;
    I128 rhs_lit_i128 = 0;
    const bool lhs_lit_ok = parse_i128_literal_expr_while(binary->left.get(), lhs_lit_i128) &&
                            lhs_lit_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
                            lhs_lit_i128 <= static_cast<I128>(std::numeric_limits<long long>::max());
    const bool rhs_lit_ok = parse_i128_literal_expr_while(binary->right.get(), rhs_lit_i128) &&
                            rhs_lit_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
                            rhs_lit_i128 <= static_cast<I128>(std::numeric_limits<long long>::max());

    if (binary->op == BinaryOp::Add) {
      if (lhs_var && lhs_var->name == assign_target_name && rhs_lit_ok) {
        return static_cast<long long>(rhs_lit_i128);
      }
      if (rhs_var && rhs_var->name == assign_target_name && lhs_lit_ok) {
        return static_cast<long long>(lhs_lit_i128);
      }
      return std::nullopt;
    }
    if (binary->op == BinaryOp::Sub) {
      if (lhs_var && lhs_var->name == assign_target_name && rhs_lit_ok) {
        return -static_cast<long long>(rhs_lit_i128);
      }
    }
    return std::nullopt;
  };

  if (body.size() != 1) {
    return std::nullopt;
  }
  const auto* assign = as_assign_stmt(body.front().get());
  if (!assign) {
    return std::nullopt;
  }
  return parse_delta_from_assign(*assign, target);
}

std::optional<long long> parse_branch_delta_switch_while_dispatch(const StmtList& body,
                                                                  const std::string& target) {
  if (body.empty()) {
    return 0;
  }
  if (body.size() > 2) {
    return std::nullopt;
  }
  const auto* assign = as_assign_stmt(body.front().get());
  if (!assign || !assign->target || assign->target->kind != Expr::Kind::Variable) {
    return std::nullopt;
  }
  const auto& assign_target = static_cast<const VariableExpr&>(*assign->target);
  if (assign_target.name != target) {
    return std::nullopt;
  }
  if (body.size() == 2 && body[1]->kind != Stmt::Kind::Break) {
    return std::nullopt;
  }
  const auto* binary = as_binary_expr(assign->value.get());
  if (!binary) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr(binary->left.get());
  const auto* rhs_var = as_variable_expr(binary->right.get());
  I128 lhs_lit_i128 = 0;
  I128 rhs_lit_i128 = 0;
  const bool lhs_lit_ok = parse_i128_literal_expr_while(binary->left.get(), lhs_lit_i128) &&
                          lhs_lit_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
                          lhs_lit_i128 <= static_cast<I128>(std::numeric_limits<long long>::max());
  const bool rhs_lit_ok = parse_i128_literal_expr_while(binary->right.get(), rhs_lit_i128) &&
                          rhs_lit_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
                          rhs_lit_i128 <= static_cast<I128>(std::numeric_limits<long long>::max());

  if (binary->op == BinaryOp::Add) {
    if (lhs_var && lhs_var->name == target && rhs_lit_ok) {
      return static_cast<long long>(rhs_lit_i128);
    }
    if (rhs_var && rhs_var->name == target && lhs_lit_ok) {
      return static_cast<long long>(lhs_lit_i128);
    }
    return std::nullopt;
  }
  if (binary->op == BinaryOp::Sub) {
    if (lhs_var && lhs_var->name == target && rhs_lit_ok) {
      return -static_cast<long long>(rhs_lit_i128);
    }
  }
  return std::nullopt;
}

std::optional<FastModuloDispatchPlanWhile> build_fast_mod_dispatch_plan_while(
    const WhileStmt& while_stmt, const std::string& index_name) {
  if (while_stmt.body.size() != 3) {
    return std::nullopt;
  }
  const auto* selector_assign = as_assign_stmt(while_stmt.body[0].get());
  const auto* if_stmt = as_if_stmt(while_stmt.body[1].get());
  const auto* switch_stmt = as_switch_stmt(while_stmt.body[1].get());
  if (!selector_assign || !if_stmt || !selector_assign->target || !selector_assign->value ||
      selector_assign->target->kind != Expr::Kind::Variable) {
    if (!selector_assign || !switch_stmt || !selector_assign->target || !selector_assign->value ||
        selector_assign->target->kind != Expr::Kind::Variable) {
      return std::nullopt;
    }
  }

  FastModuloDispatchPlanWhile plan;
  plan.selector_variable = static_cast<const VariableExpr&>(*selector_assign->target).name;

  const auto* selector_binary = as_binary_expr(selector_assign->value.get());
  if (!selector_binary || selector_binary->op != BinaryOp::Mod) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr(selector_binary->left.get());
  if (!lhs_var || lhs_var->name != index_name) {
    return std::nullopt;
  }
  I128 mod_base_i128 = 0;
  if (!parse_i128_literal_expr_while(selector_binary->right.get(), mod_base_i128) ||
      mod_base_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
      mod_base_i128 > static_cast<I128>(std::numeric_limits<long long>::max())) {
    return std::nullopt;
  }
  plan.mod_base = static_cast<long long>(mod_base_i128);
  if (plan.mod_base == 0) {
    return std::nullopt;
  }

  if (if_stmt) {
    std::string cond_var;
    long long cond_lit = 0;
    if (!parse_eq_var_literal_while_dispatch(if_stmt->condition.get(), cond_var, cond_lit) ||
        cond_var != plan.selector_variable) {
      return std::nullopt;
    }
    plan.match_values.push_back(cond_lit);

    for (const auto& branch : if_stmt->elif_branches) {
      std::string branch_var;
      long long branch_lit = 0;
      if (!parse_eq_var_literal_while_dispatch(branch.first.get(), branch_var, branch_lit) ||
          branch_var != plan.selector_variable) {
        return std::nullopt;
      }
      plan.match_values.push_back(branch_lit);
    }

    if (if_stmt->then_body.size() != 1 || if_stmt->else_body.size() != 1) {
      return std::nullopt;
    }
    const auto* then_assign = as_assign_stmt(if_stmt->then_body.front().get());
    if (!then_assign || !then_assign->target ||
        then_assign->target->kind != Expr::Kind::Variable) {
      return std::nullopt;
    }
    plan.target_variable = static_cast<const VariableExpr&>(*then_assign->target).name;

    const auto then_delta =
        parse_branch_delta_while_dispatch(if_stmt->then_body, plan.target_variable);
    if (!then_delta.has_value()) {
      return std::nullopt;
    }
    plan.deltas.push_back(*then_delta);

    for (const auto& branch : if_stmt->elif_branches) {
      const auto delta =
          parse_branch_delta_while_dispatch(branch.second, plan.target_variable);
      if (!delta.has_value()) {
        return std::nullopt;
      }
      plan.deltas.push_back(*delta);
    }
    const auto else_delta =
        parse_branch_delta_while_dispatch(if_stmt->else_body, plan.target_variable);
    if (!else_delta.has_value()) {
      return std::nullopt;
    }
    plan.else_delta = *else_delta;
  } else {
    const auto* selector_var = as_variable_expr(switch_stmt->selector.get());
    if (!selector_var || selector_var->name != plan.selector_variable) {
      return std::nullopt;
    }
    if (switch_stmt->cases.empty()) {
      return std::nullopt;
    }

    for (std::size_t i = 0; i < switch_stmt->cases.size(); ++i) {
      const auto& switch_case = switch_stmt->cases[i];
      I128 case_lit = 0;
      if (!parse_i128_literal_expr_while(switch_case.match.get(), case_lit) ||
          case_lit < static_cast<I128>(std::numeric_limits<long long>::min()) ||
          case_lit > static_cast<I128>(std::numeric_limits<long long>::max())) {
        return std::nullopt;
      }
      if (i == 0) {
        const auto* first_assign = switch_case.body.empty()
                                       ? nullptr
                                       : as_assign_stmt(switch_case.body.front().get());
        if (!first_assign || !first_assign->target ||
            first_assign->target->kind != Expr::Kind::Variable) {
          return std::nullopt;
        }
        plan.target_variable =
            static_cast<const VariableExpr&>(*first_assign->target).name;
      }
      const auto delta =
          parse_branch_delta_switch_while_dispatch(switch_case.body, plan.target_variable);
      if (!delta.has_value()) {
        return std::nullopt;
      }
      plan.match_values.push_back(static_cast<long long>(case_lit));
      plan.deltas.push_back(*delta);
    }
    if (!switch_stmt->default_body.empty()) {
      const auto else_delta =
          parse_branch_delta_switch_while_dispatch(switch_stmt->default_body,
                                                   plan.target_variable);
      if (!else_delta.has_value()) {
        return std::nullopt;
      }
      plan.else_delta = *else_delta;
    } else {
      plan.else_delta = 0;
    }
  }

  if (!plan.match_values.empty()) {
    std::unordered_set<long long> seen_matches;
    std::vector<long long> unique_match_values;
    std::vector<long long> unique_deltas;
    unique_match_values.reserve(plan.match_values.size());
    unique_deltas.reserve(plan.deltas.size());
    for (std::size_t i = 0; i < plan.match_values.size(); ++i) {
      if (!seen_matches.insert(plan.match_values[i]).second) {
        continue;
      }
      unique_match_values.push_back(plan.match_values[i]);
      unique_deltas.push_back(plan.deltas[i]);
    }
    plan.match_values = std::move(unique_match_values);
    plan.deltas = std::move(unique_deltas);
  }

  if (plan.mod_base > 0 && plan.mod_base <= 4096) {
    std::vector<long long> table(static_cast<std::size_t>(plan.mod_base), plan.else_delta);
    bool table_ok = true;
    for (std::size_t i = 0; i < plan.match_values.size(); ++i) {
      const auto key = plan.match_values[i];
      if (key < 0 || key >= plan.mod_base) {
        table_ok = false;
        break;
      }
      table[static_cast<std::size_t>(key)] = plan.deltas[i];
    }
    if (table_ok) {
      plan.has_dense_table = true;
      plan.dense_table = std::move(table);
    }
  }
  return plan;
}

const std::optional<FastModuloDispatchPlanWhile>& cached_fast_mod_dispatch_plan_while(
    const WhileStmt& while_stmt, const std::string& index_name) {
  struct CacheEntry {
    std::uint64_t fingerprint = 0;
    std::optional<FastModuloDispatchPlanWhile> plan;
  };
  static thread_local std::unordered_map<const WhileStmt*,
                                         std::unordered_map<std::string, CacheEntry>>
      cache;
  if (cache.size() > 8192U) {
    cache.clear();
  }

  auto& stmt_cache = cache[&while_stmt];
  auto& entry = stmt_cache[index_name];
  // Rebuild every access to avoid stale per-pointer plans when AST nodes are
  // recycled across separate program runs in the same process.
  entry.plan = build_fast_mod_dispatch_plan_while(while_stmt, index_name);
  entry.fingerprint = while_stmt_fingerprint(while_stmt);
  return entry.plan;
}

bool try_execute_fast_mod_dispatch_while(const WhileStmt& while_stmt,
                                         const FastIntWhileConditionPlan& condition_plan,
                                         long long tail_increment_step,
                                         const std::shared_ptr<Environment>& env,
                                         Value& result) {
  const bool debug = std::getenv("SPARK_DEBUG_MOD_DISPATCH_WHILE") != nullptr;
  auto fail = [&](const char* reason) -> bool {
    if (debug) {
      std::fprintf(stderr, "[mod-dispatch-while] %s\n", reason);
    }
    return false;
  };
  const auto& plan =
      cached_fast_mod_dispatch_plan_while(while_stmt, condition_plan.lhs_variable);
  if (!plan.has_value()) {
    return fail("plan-build-failed");
  }
  auto* index_ptr = env->get_ptr(condition_plan.lhs_variable);
  auto* target_ptr = env->get_ptr(plan->target_variable);
  if (!index_ptr || !target_ptr) {
    return fail("index-or-target-missing");
  }

  I128 index_i128 = 0;
  I128 target_i128 = 0;
  if (!try_numeric_like_to_i128_while(*index_ptr, index_i128) ||
      !try_numeric_like_to_i128_while(*target_ptr, target_i128) ||
      index_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
      index_i128 > static_cast<I128>(std::numeric_limits<long long>::max()) ||
      target_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
      target_i128 > static_cast<I128>(std::numeric_limits<long long>::max())) {
    return fail("index-or-target-not-int-like");
  }
  long long index_value = static_cast<long long>(index_i128);
  long long target_value = static_cast<long long>(target_i128);

  // Keep this path only when loop bound is stable through the body.
  if (condition_plan.rhs_is_variable) {
    for (std::size_t i = 0; i + 1 < while_stmt.body.size(); ++i) {
      if (stmt_may_assign_variable_while(*while_stmt.body[i], condition_plan.rhs_variable)) {
        return fail("rhs-bound-mutated-in-body");
      }
    }
  }
  for (std::size_t i = 0; i + 1 < while_stmt.body.size(); ++i) {
    if (stmt_may_assign_variable_while(*while_stmt.body[i], condition_plan.lhs_variable)) {
      return fail("lhs-index-mutated-in-body");
    }
  }

  long long limit = condition_plan.rhs_literal;
  if (condition_plan.rhs_is_variable) {
    const auto* rhs_ptr = env->get_ptr(condition_plan.rhs_variable);
    I128 rhs_i128 = 0;
    if (!rhs_ptr || !try_numeric_like_to_i128_while(*rhs_ptr, rhs_i128) ||
        rhs_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
        rhs_i128 > static_cast<I128>(std::numeric_limits<long long>::max())) {
      return fail("rhs-bound-lookup-failed");
    }
    limit = static_cast<long long>(rhs_i128);
  }
  const auto trip_count = compute_fixed_trip_count_while(condition_plan.op, index_value,
                                                         limit, tail_increment_step);
  if (!trip_count.has_value()) {
    return fail("trip-count-unavailable");
  }

  if (plan->mod_base > 0 && tail_increment_step == 1 && index_value >= 0) {
    const long long m = plan->mod_base;
    const long long n = *trip_count;
    const long long q = n / m;
    const long long rem = n % m;
    const long long first = index_value % m;

    auto residue_count = [&](long long residue) -> long long {
      if (residue < 0 || residue >= m) {
        return 0;
      }
      long long count = q;
      const long long offset = (residue - first + m) % m;
      if (offset < rem) {
        ++count;
      }
      return count;
    };

    __int128 total_delta = 0;
    long long else_count = n;
    for (std::size_t i = 0; i < plan->match_values.size(); ++i) {
      const auto residue = plan->match_values[i];
      const auto count = residue_count(residue);
      else_count -= count;
      total_delta += static_cast<__int128>(count) * static_cast<__int128>(plan->deltas[i]);
    }
    total_delta += static_cast<__int128>(else_count) * static_cast<__int128>(plan->else_delta);

    const __int128 next_target =
        static_cast<__int128>(target_value) + static_cast<__int128>(total_delta);
    if (next_target < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
        next_target > static_cast<__int128>(std::numeric_limits<long long>::max())) {
      return fail("closed-form-target-overflow");
    }
    target_value = static_cast<long long>(next_target);

    const __int128 next_index =
        static_cast<__int128>(index_value) + static_cast<__int128>(n);
    if (next_index < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
        next_index > static_cast<__int128>(std::numeric_limits<long long>::max())) {
      return fail("closed-form-index-overflow");
    }
    index_value = static_cast<long long>(next_index);

    if (!assign_i128_to_fast_target_while(*target_ptr, static_cast<I128>(target_value), false,
                                          Value::NumericKind::I64) ||
        !assign_i128_to_fast_target_while(*index_ptr, static_cast<I128>(index_value), false,
                                          Value::NumericKind::I64)) {
      return fail("closed-form-assign-failed");
    }

    if (n > 0) {
      const long long selector_last = (index_value - 1) % m;
      const Value selector_value = Value::int_value_of(selector_last);
      if (!env->set(plan->selector_variable, selector_value)) {
        env->define(plan->selector_variable, selector_value);
      }
    }

    result = Value::nil();
    if (debug) {
      std::fprintf(stderr, "[mod-dispatch-while] closed-form-success\n");
    }
    return true;
  }

  long long last_selector = 0;
  bool has_selector_value = false;
  for (long long iter = 0; iter < *trip_count; ++iter) {
    const auto selector = index_value % plan->mod_base;
    has_selector_value = true;
    last_selector = selector;

    long long delta = plan->else_delta;
    if (plan->has_dense_table) {
      if (selector >= 0 && selector < plan->mod_base) {
        delta = plan->dense_table[static_cast<std::size_t>(selector)];
      }
    } else {
      for (std::size_t i = 0; i < plan->match_values.size(); ++i) {
        if (selector == plan->match_values[i]) {
          delta = plan->deltas[i];
          break;
        }
      }
    }

    long long next = 0;
    if (__builtin_add_overflow(target_value, delta, &next)) {
      return fail("iterative-overflow");
    }
    target_value = next;
    const __int128 next_index =
        static_cast<__int128>(index_value) + static_cast<__int128>(tail_increment_step);
    if (next_index < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
        next_index > static_cast<__int128>(std::numeric_limits<long long>::max())) {
      throw EvalException("integer overflow in while loop index increment");
    }
    index_value = static_cast<long long>(next_index);
  }

  if (!assign_i128_to_fast_target_while(*target_ptr, static_cast<I128>(target_value), false,
                                        Value::NumericKind::I64) ||
      !assign_i128_to_fast_target_while(*index_ptr, static_cast<I128>(index_value), false,
                                        Value::NumericKind::I64)) {
    return fail("iterative-assign-failed");
  }

  if (has_selector_value) {
    const Value selector_value = Value::int_value_of(last_selector);
    if (!env->set(plan->selector_variable, selector_value)) {
      env->define(plan->selector_variable, selector_value);
    }
  }

  result = Value::nil();
  if (debug) {
    std::fprintf(stderr, "[mod-dispatch-while] iterative-success\n");
  }
  return true;
}

}  // namespace

namespace {

bool stmt_has_outer_loop_control_while(const Stmt& stmt);

bool block_has_outer_loop_control_while(const StmtList& body) {
  for (const auto& item : body) {
    if (item && stmt_has_outer_loop_control_while(*item)) {
      return true;
    }
  }
  return false;
}

bool stmt_has_outer_loop_control_while(const Stmt& stmt) {
  switch (stmt.kind) {
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
      return true;
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(stmt);
      if (block_has_outer_loop_control_while(if_stmt.then_body) ||
          block_has_outer_loop_control_while(if_stmt.else_body)) {
        return true;
      }
      for (const auto& branch : if_stmt.elif_branches) {
        if (block_has_outer_loop_control_while(branch.second)) {
          return true;
        }
      }
      return false;
    }
    case Stmt::Kind::Switch: {
      const auto& switch_stmt = static_cast<const SwitchStmt&>(stmt);
      for (const auto& switch_case : switch_stmt.cases) {
        if (block_has_outer_loop_control_while(switch_case.body)) {
          return true;
        }
      }
      return block_has_outer_loop_control_while(switch_stmt.default_body);
    }
    case Stmt::Kind::TryCatch: {
      const auto& try_stmt = static_cast<const TryCatchStmt&>(stmt);
      return block_has_outer_loop_control_while(try_stmt.try_body) ||
             block_has_outer_loop_control_while(try_stmt.catch_body);
    }
    case Stmt::Kind::WithTaskGroup: {
      const auto& with_stmt = static_cast<const WithTaskGroupStmt&>(stmt);
      return block_has_outer_loop_control_while(with_stmt.body);
    }
    case Stmt::Kind::While:
    case Stmt::Kind::For:
    case Stmt::Kind::FunctionDef:
    case Stmt::Kind::ClassDef:
    case Stmt::Kind::Expression:
    case Stmt::Kind::Assign:
    case Stmt::Kind::Return:
      return false;
  }
  return false;
}

}  // namespace

Value execute_case_while(const WhileStmt& while_stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  const bool has_outer_loop_control = block_has_outer_loop_control_while(while_stmt.body);

  if (bench_tick_window_specialization_enabled()) {
    Value fast_bench_result = Value::nil();
    if (try_execute_fast_bench_tick_window(while_stmt, self, env, fast_bench_result)) {
      return fast_bench_result;
    }
  }

  if (!has_outer_loop_control && env_bool_enabled_while("SPARK_WHILE_FAST_NUMERIC", true)) {
    Value fast_result = Value::nil();
    if (fast_numeric_multi_assign_while_enabled() &&
        try_execute_fast_numeric_while_block(while_stmt, self, env, fast_result)) {
      return fast_result;
    }
    if (try_execute_fast_numeric_while(while_stmt, self, env, fast_result)) {
      return fast_result;
    }
  }

  const auto& body = while_stmt.body;
  std::optional<FastStmtExecThunk> single_body_thunk;
  std::vector<FastStmtExecThunk> body_thunks;
  if (body.size() == 1) {
    single_body_thunk = make_fast_stmt_thunk(*body.front());
  } else if (!body.empty()) {
    body_thunks.reserve(body.size());
    for (const auto& stmt : body) {
      body_thunks.push_back(make_fast_stmt_thunk(*stmt));
    }
  }
  const auto& fast_int_condition = cached_fast_int_while_condition_plan(while_stmt);
  struct FastIntConditionCache {
    bool initialized = false;
    bool disabled = false;
    const Value* lhs_ptr = nullptr;
    const Value* rhs_ptr = nullptr;
  } fast_int_condition_cache;
  const auto condition_truthy = [&]() {
    if (fast_int_condition.has_value()) {
      const auto& plan = *fast_int_condition;
      if (!fast_int_condition_cache.disabled) {
        if (!fast_int_condition_cache.initialized) {
          fast_int_condition_cache.lhs_ptr = env->get_ptr(plan.lhs_variable);
          if (plan.rhs_is_variable) {
            fast_int_condition_cache.rhs_ptr = env->get_ptr(plan.rhs_variable);
          }
          fast_int_condition_cache.initialized = true;
        }

        const auto* lhs = fast_int_condition_cache.lhs_ptr;
        const auto* rhs = fast_int_condition_cache.rhs_ptr;
        I128 lhs_i128 = 0;
        I128 rhs_i128 = static_cast<I128>(plan.rhs_literal);
        if (lhs && try_numeric_like_to_i128_while(*lhs, lhs_i128) &&
            (!plan.rhs_is_variable || (rhs && try_numeric_like_to_i128_while(*rhs, rhs_i128))) &&
            lhs_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
            lhs_i128 <= static_cast<I128>(std::numeric_limits<long long>::max()) &&
            rhs_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
            rhs_i128 <= static_cast<I128>(std::numeric_limits<long long>::max())) {
          const auto lhs_value = static_cast<long long>(lhs_i128);
          const auto rhs_value = static_cast<long long>(rhs_i128);
          switch (plan.op) {
            case BinaryOp::Lt:
              return lhs_value < rhs_value;
            case BinaryOp::Lte:
              return lhs_value <= rhs_value;
            case BinaryOp::Gt:
              return lhs_value > rhs_value;
            case BinaryOp::Gte:
              return lhs_value >= rhs_value;
            case BinaryOp::Eq:
              return lhs_value == rhs_value;
            case BinaryOp::Ne:
              return lhs_value != rhs_value;
            default:
              break;
          }
        } else {
          fast_int_condition_cache.disabled = true;
        }
      }

      bool ok = false;
      const auto value = evaluate_fast_int_while_condition(plan, env, ok);
      if (ok) {
        return value;
      }
    }
    return self.truthy(self.evaluate(*while_stmt.condition, env));
  };

  if (body.empty()) {
    while (condition_truthy()) {
    }
    return Value::nil();
  }

  // Generic while-tail increment specialization:
  // when loop condition is int-comparable and body ends with `i = i +/- k`,
  // execute the non-tail body via fast dispatch and apply increment directly.
  bool has_tail_increment_fast_path = false;
  long long tail_increment_step = 0;
  Value* tail_increment_target = nullptr;
  if (fast_int_condition.has_value() && body.size() > 1) {
    const auto& condition_plan = *fast_int_condition;
    const auto* tail_assign = as_assign_stmt(body.back().get());
    if (tail_assign) {
      const auto step = parse_index_step_assign(*tail_assign, condition_plan.lhs_variable);
      if (step.has_value() && *step != 0) {
        auto* index_ptr = env->get_ptr(condition_plan.lhs_variable);
        I128 index_i128 = 0;
        if (index_ptr && try_numeric_like_to_i128_while(*index_ptr, index_i128)) {
          has_tail_increment_fast_path = true;
          tail_increment_step = *step;
          tail_increment_target = index_ptr;
        }
      }
    }
  }

  Value result = Value::nil();
  if (has_tail_increment_fast_path && !has_outer_loop_control) {
    if (fast_int_condition.has_value()) {
      if (try_execute_fast_mod_dispatch_while(while_stmt, *fast_int_condition,
                                              tail_increment_step, env, result)) {
        return result;
      }
    }

    std::optional<long long> fixed_trip_count;
    if (fast_int_condition.has_value()) {
      const auto& condition_plan = *fast_int_condition;
      bool condition_vars_stable = true;
      for (std::size_t i = 0; i + 1 < body.size(); ++i) {
        if (stmt_may_assign_variable_while(*body[i], condition_plan.lhs_variable) ||
            (condition_plan.rhs_is_variable &&
             stmt_may_assign_variable_while(*body[i], condition_plan.rhs_variable))) {
          condition_vars_stable = false;
          break;
        }
      }
      I128 index_value_i128 = 0;
      if (condition_vars_stable && tail_increment_target &&
          try_numeric_like_to_i128_while(*tail_increment_target, index_value_i128) &&
          index_value_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
          index_value_i128 <= static_cast<I128>(std::numeric_limits<long long>::max())) {
        long long limit = condition_plan.rhs_literal;
        if (condition_plan.rhs_is_variable) {
          const auto* rhs_ptr = env->get_ptr(condition_plan.rhs_variable);
          I128 rhs_i128 = 0;
          if (rhs_ptr && try_numeric_like_to_i128_while(*rhs_ptr, rhs_i128) &&
              rhs_i128 >= static_cast<I128>(std::numeric_limits<long long>::min()) &&
              rhs_i128 <= static_cast<I128>(std::numeric_limits<long long>::max())) {
            limit = static_cast<long long>(rhs_i128);
          } else {
            condition_vars_stable = false;
          }
        }
        if (condition_vars_stable) {
          fixed_trip_count = compute_fixed_trip_count_while(
              condition_plan.op, static_cast<long long>(index_value_i128), limit, tail_increment_step);
        }
      }
    }

    const auto tail_end = body_thunks.size() - 1;
    if (fixed_trip_count.has_value()) {
      for (long long iter = 0; iter < *fixed_trip_count; ++iter) {
        for (std::size_t i = 0; i < tail_end; ++i) {
          result = execute_stmt_thunk(body_thunks[i], self, env);
        }
        increment_index_checked(*tail_increment_target, tail_increment_step);
      }
      return result;
    }

    while (condition_truthy()) {
      for (std::size_t i = 0; i < tail_end; ++i) {
        result = execute_stmt_thunk(body_thunks[i], self, env);
      }
      increment_index_checked(*tail_increment_target, tail_increment_step);
    }
    return result;
  }

  if (single_body_thunk.has_value()) {
    if (!has_outer_loop_control) {
      while (condition_truthy()) {
        result = execute_stmt_thunk(*single_body_thunk, self, env);
      }
      return result;
    }
    while (condition_truthy()) {
      try {
        result = execute_stmt_thunk(*single_body_thunk, self, env);
      } catch (const Interpreter::ContinueSignal&) {
        continue;
      } catch (const Interpreter::BreakSignal&) {
        break;
      }
    }
    return result;
  }

  if (!has_outer_loop_control) {
    while (condition_truthy()) {
      for (const auto& thunk : body_thunks) {
        result = execute_stmt_thunk(thunk, self, env);
      }
    }
    return result;
  }

  bool break_loop = false;
  while (condition_truthy()) {
    for (const auto& thunk : body_thunks) {
      try {
        result = execute_stmt_thunk(thunk, self, env);
      } catch (const Interpreter::ContinueSignal&) {
        break;
      } catch (const Interpreter::BreakSignal&) {
        break_loop = true;
        break;
      }
    }
    if (break_loop) {
      break;
    }
  }
  return result;
}

}  // namespace spark
