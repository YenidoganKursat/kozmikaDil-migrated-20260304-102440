#include "../internal_helpers.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <optional>
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
  // Experimental multi-assign superinstruction path.
  // Keep default off until broad benchmark coverage shows stable win.
  static const bool enabled = env_flag_enabled("SPARK_WHILE_FAST_NUMERIC_MULTI", false);
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

std::optional<long long> literal_i64(const Expr* expr) {
  const auto* number = as_number_expr(expr);
  if (!number) {
    return std::nullopt;
  }
  return number_to_i64(*number);
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

bool evaluate_fast_int_while_condition(const FastIntWhileConditionPlan& plan,
                                       const std::shared_ptr<Environment>& env,
                                       bool& ok) {
  ok = false;
  const auto* lhs = env->get_ptr(plan.lhs_variable);
  if (!lhs || lhs->kind != Value::Kind::Int) {
    return false;
  }
  long long rhs_value = plan.rhs_literal;
  if (plan.rhs_is_variable) {
    const auto* rhs = env->get_ptr(plan.rhs_variable);
    if (!rhs || rhs->kind != Value::Kind::Int) {
      return false;
    }
    rhs_value = rhs->int_value;
  }

  ok = true;
  switch (plan.op) {
    case BinaryOp::Lt:
      return lhs->int_value < rhs_value;
    case BinaryOp::Lte:
      return lhs->int_value <= rhs_value;
    case BinaryOp::Gt:
      return lhs->int_value > rhs_value;
    case BinaryOp::Gte:
      return lhs->int_value >= rhs_value;
    case BinaryOp::Eq:
      return lhs->int_value == rhs_value;
    case BinaryOp::Ne:
      return lhs->int_value != rhs_value;
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
  std::string pre_copy_target;
  if (!parse_simple_var_assign_stmt(while_stmt.body[1].get(), pre_copy_target, plan.pre_copy_source_var)) {
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
  if (plan.op_target_var != pre_copy_target) {
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
  auto* pre_copy_source_ptr = env->get_ptr(plan->pre_copy_source_var);
  if (!index_ptr || !floor_total_ptr || !raw_total_ptr || !op_target_ptr || !pre_copy_source_ptr) {
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
    out.payload = in.payload;
    out.parsed_int_valid = in.parsed_int_valid;
    out.parsed_int = in.parsed_int;
    out.parsed_float_valid = in.parsed_float_valid;
    out.parsed_float = in.parsed_float;
    ++out.revision;
    out.high_precision_cache.reset();
  };

  const bool pre_copy_int_fast =
      op_target_ptr->kind == Value::Kind::Int &&
      pre_copy_source_ptr->kind == Value::Kind::Int;
  const bool pre_copy_double_fast =
      op_target_ptr->kind == Value::Kind::Double &&
      pre_copy_source_ptr->kind == Value::Kind::Double;
  const bool pre_copy_numeric_fast =
      op_target_ptr->kind == Value::Kind::Numeric &&
      pre_copy_source_ptr->kind == Value::Kind::Numeric &&
      op_target_ptr->numeric_value && pre_copy_source_ptr->numeric_value &&
      op_target_ptr->numeric_value->kind == pre_copy_source_ptr->numeric_value->kind &&
      !numeric_kind_is_high_precision_float(op_target_ptr->numeric_value->kind);
  const bool pre_copy_numeric_any =
      op_target_ptr->kind == Value::Kind::Numeric &&
      pre_copy_source_ptr->kind == Value::Kind::Numeric &&
      op_target_ptr->numeric_value && pre_copy_source_ptr->numeric_value &&
      op_target_ptr->numeric_value->kind == pre_copy_source_ptr->numeric_value->kind;

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

  result = Value::nil();
  while (true) {
    const auto maybe_limit = resolve_limit();
    if (!maybe_limit.has_value()) {
      return false;
    }
    if (index_ptr->int_value >= *maybe_limit) {
      break;
    }

    const auto f1_raw = read_tick_raw();
    last_f1_raw = f1_raw;
    if (pre_copy_int_fast) {
      op_target_ptr->int_value = pre_copy_source_ptr->int_value;
    } else if (pre_copy_double_fast) {
      op_target_ptr->double_value = pre_copy_source_ptr->double_value;
    } else if (pre_copy_numeric_fast) {
      copy_nonhp_numeric(*op_target_ptr, *pre_copy_source_ptr);
    } else if (pre_copy_numeric_any &&
               copy_numeric_value_inplace(*op_target_ptr, *pre_copy_source_ptr)) {
      // copied
    } else {
      *op_target_ptr = *pre_copy_source_ptr;
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
      (void)eval_numeric_binary_value_inplace(plan->op, *lhs_ptr, *rhs_ptr, *op_target_ptr);
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

    const __int128 next_index = static_cast<__int128>(index_ptr->int_value) +
                                static_cast<__int128>(plan->index_step);
    if (next_index < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
        next_index > static_cast<__int128>(std::numeric_limits<long long>::max())) {
      throw EvalException("integer overflow in while loop index increment");
    }
    index_ptr->int_value = static_cast<long long>(next_index);
  }

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
    Increment,
  };

  Kind kind = Kind::Assign;
  std::size_t assign_index = 0;
};

struct FastNumericWhileBlockPlan {
  std::string index_name;
  long long index_step = 1;
  bool limit_is_variable = false;
  long long limit_value = 0;
  std::string limit_variable;
  std::vector<FastNumericWhilePlan> assign_plans;
  std::vector<FastNumericWhileBlockStep> steps;
};

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
    if (!assign) {
      return std::nullopt;
    }
    if (const auto step = parse_index_step_assign(*assign, plan.index_name); step.has_value()) {
      if (seen_increment || *step <= 0) {
        return std::nullopt;
      }
      plan.index_step = *step;
      seen_increment = true;
      plan.steps.push_back(FastNumericWhileBlockStep{
          .kind = FastNumericWhileBlockStep::Kind::Increment,
          .assign_index = 0,
      });
      continue;
    }

    FastNumericWhilePlan assign_plan;
    if (!parse_fast_numeric_assign(*assign, plan.index_name, assign_plan)) {
      return std::nullopt;
    }
    if (assign_plan.accumulator_name == plan.index_name) {
      return std::nullopt;
    }
    const std::size_t assign_index = plan.assign_plans.size();
    plan.assign_plans.push_back(std::move(assign_plan));
    plan.steps.push_back(FastNumericWhileBlockStep{
        .kind = FastNumericWhileBlockStep::Kind::Assign,
        .assign_index = assign_index,
    });
  }

  if (!seen_increment || plan.assign_plans.empty()) {
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
  if (index.kind != Value::Kind::Int) {
    throw EvalException("fast while path expects integer loop index");
  }
  if (index.int_value == std::numeric_limits<long long>::max()) {
    throw EvalException("integer overflow in while loop index increment");
  }
  ++index.int_value;
}

void increment_index_checked(Value& index, long long step) {
  if (index.kind != Value::Kind::Int) {
    throw EvalException("fast while path expects integer loop index");
  }
  const __int128 next = static_cast<__int128>(index.int_value) + static_cast<__int128>(step);
  if (next < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
      next > static_cast<__int128>(std::numeric_limits<long long>::max())) {
    throw EvalException("integer overflow in while loop index increment");
  }
  index.int_value = static_cast<long long>(next);
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
  FastNumericWhileOperandRef lhs;
  FastNumericWhileOperandRef rhs;
};

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

bool try_execute_fast_numeric_while_block(const WhileStmt& while_stmt, Interpreter& self,
                                          const std::shared_ptr<Environment>& env,
                                          Value& result) {
  const auto plan = build_fast_numeric_while_block_plan(while_stmt);
  if (!plan.has_value()) {
    return false;
  }

  auto* index_ptr = env->get_ptr(plan->index_name);
  if (!index_ptr || index_ptr->kind != Value::Kind::Int) {
    return false;
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
    if (!runtime.target || !is_numeric_kind(*runtime.target)) {
      return false;
    }

    if (!initialize_fast_numeric_operand_ref(runtime.lhs, assign_plan.lhs_is_variable,
                                             assign_plan.lhs_variable, assign_plan.lhs_expr,
                                             dynamic_names, self, env) ||
        !initialize_fast_numeric_operand_ref(runtime.rhs, assign_plan.rhs_is_variable,
                                             assign_plan.rhs_variable, assign_plan.rhs_expr,
                                             dynamic_names, self, env)) {
      return false;
    }

    runtime_assigns.push_back(std::move(runtime));
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

      if (step.assign_index >= runtime_assigns.size()) {
        return false;
      }
      auto& runtime = runtime_assigns[step.assign_index];
      const auto* lhs_ptr = resolve_fast_numeric_operand_ref(runtime.lhs, env);
      const auto* rhs_ptr = resolve_fast_numeric_operand_ref(runtime.rhs, env);
      if (!lhs_ptr || !rhs_ptr || !is_numeric_kind(*lhs_ptr) || !is_numeric_kind(*rhs_ptr) ||
          !runtime.target || !is_numeric_kind(*runtime.target)) {
        return false;
      }

      if (!eval_numeric_binary_value_inplace(runtime.op, *lhs_ptr, *rhs_ptr, *runtime.target)) {
        *runtime.target = eval_numeric_binary_value(runtime.op, *lhs_ptr, *rhs_ptr);
      }
      result = *runtime.target;
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

}  // namespace

Value execute_case_while(const WhileStmt& while_stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  if (bench_tick_window_specialization_enabled()) {
    Value fast_bench_result = Value::nil();
    if (try_execute_fast_bench_tick_window(while_stmt, self, env, fast_bench_result)) {
      return fast_bench_result;
    }
  }

  if (env_bool_enabled_while("SPARK_WHILE_FAST_NUMERIC", true)) {
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
  const auto fast_int_condition = build_fast_int_while_condition_plan(while_stmt);
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
        if (lhs && lhs->kind == Value::Kind::Int &&
            (!plan.rhs_is_variable || (rhs && rhs->kind == Value::Kind::Int))) {
          const auto lhs_value = lhs->int_value;
          const auto rhs_value = plan.rhs_is_variable ? rhs->int_value : plan.rhs_literal;
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

  Value result = Value::nil();
  if (body.size() == 1) {
    const auto& only = *body.front();
    while (condition_truthy()) {
      result = self.execute(only, env);
    }
    return result;
  }

  while (condition_truthy()) {
    for (const auto& child : body) {
      result = self.execute(*child, env);
    }
  }
  return result;
}

}  // namespace spark
