#include "../internal_helpers.h"

#include <cstdio>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spark {

namespace {

using I128 = __int128_t;

bool env_bool_enabled_for(const char* name, bool fallback) {
  (void)name;
  (void)fallback;
  // Single runtime policy: always keep the fastest verified for-range paths enabled.
  return true;
}

bool is_numeric_arithmetic_op_for(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

bool is_commutative_numeric_op_for(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Mul;
}

const VariableExpr* as_variable_expr_for(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return static_cast<const VariableExpr*>(expr);
}

const NumberExpr* as_number_expr_for(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Number) {
    return nullptr;
  }
  return static_cast<const NumberExpr*>(expr);
}

std::optional<long long> number_to_i64_for(const NumberExpr& number) {
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

const BinaryExpr* as_binary_expr_for(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Binary) {
    return nullptr;
  }
  return static_cast<const BinaryExpr*>(expr);
}

const AssignStmt* as_assign_stmt_for(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::Assign) {
    return nullptr;
  }
  return static_cast<const AssignStmt*>(stmt);
}

struct RangeSpec {
  long long start = 0;
  long long stop = 0;
  long long step = 1;
};

long long range_iteration_count(const RangeSpec& spec);

bool try_numeric_like_to_i128_for(const Value& value, I128& out) {
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

bool assign_i128_to_intlike_for(Value& target, I128 value) {
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

const IfStmt* as_if_stmt_for(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::If) {
    return nullptr;
  }
  return static_cast<const IfStmt*>(stmt);
}

const SwitchStmt* as_switch_stmt_for(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::Switch) {
    return nullptr;
  }
  return static_cast<const SwitchStmt*>(stmt);
}

std::optional<long long> extract_int_literal_expr_for(const Expr* expr) {
  if (!expr) {
    return std::nullopt;
  }
  if (const auto* number = as_number_expr_for(expr)) {
    return number_to_i64_for(*number);
  }
  if (expr->kind == Expr::Kind::Unary) {
    const auto& unary = static_cast<const UnaryExpr&>(*expr);
    if (unary.op != UnaryOp::Neg || !unary.operand) {
      return std::nullopt;
    }
    const auto inner = extract_int_literal_expr_for(unary.operand.get());
    if (!inner.has_value()) {
      return std::nullopt;
    }
    return -*inner;
  }
  if (expr->kind == Expr::Kind::Call) {
    const auto& call = static_cast<const CallExpr&>(*expr);
    if (!call.callee || call.callee->kind != Expr::Kind::Variable || call.args.size() != 1 ||
        !call.args[0]) {
      return std::nullopt;
    }
    const auto& callee = static_cast<const VariableExpr&>(*call.callee);
    try {
      const auto kind = numeric_kind_from_name(callee.name);
      if (!numeric_kind_is_int(kind)) {
        return std::nullopt;
      }
    } catch (const EvalException&) {
      return std::nullopt;
    }
    return extract_int_literal_expr_for(call.args[0].get());
  }
  return std::nullopt;
}

bool parse_eq_var_literal_for(const Expr* condition, std::string& out_variable,
                              long long& out_literal) {
  const auto* binary = as_binary_expr_for(condition);
  if (!binary || binary->op != BinaryOp::Eq) {
    return false;
  }
  if (const auto* lhs_var = as_variable_expr_for(binary->left.get())) {
    const auto rhs_lit = extract_int_literal_expr_for(binary->right.get());
    if (!rhs_lit.has_value()) {
      return false;
    }
    out_variable = lhs_var->name;
    out_literal = *rhs_lit;
    return true;
  }
  if (const auto* rhs_var = as_variable_expr_for(binary->right.get())) {
    const auto lhs_lit = extract_int_literal_expr_for(binary->left.get());
    if (!lhs_lit.has_value()) {
      return false;
    }
    out_variable = rhs_var->name;
    out_literal = *lhs_lit;
    return true;
  }
  return false;
}

std::optional<long long> parse_branch_delta_for(const StmtList& body,
                                                 const std::string& target) {
  if (body.size() != 1) {
    return std::nullopt;
  }
  const auto* assign = as_assign_stmt_for(body.front().get());
  if (!assign || !assign->target || assign->target->kind != Expr::Kind::Variable || !assign->value) {
    return std::nullopt;
  }
  const auto& assign_target = static_cast<const VariableExpr&>(*assign->target);
  if (assign_target.name != target) {
    return std::nullopt;
  }
  const auto* binary = as_binary_expr_for(assign->value.get());
  if (!binary) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr_for(binary->left.get());
  const auto* rhs_var = as_variable_expr_for(binary->right.get());
  const auto lhs_lit = extract_int_literal_expr_for(binary->left.get());
  const auto rhs_lit = extract_int_literal_expr_for(binary->right.get());
  if (binary->op == BinaryOp::Add) {
    if (lhs_var && lhs_var->name == target && rhs_lit.has_value()) {
      return *rhs_lit;
    }
    if (rhs_var && rhs_var->name == target && lhs_lit.has_value()) {
      return *lhs_lit;
    }
    return std::nullopt;
  }
  if (binary->op == BinaryOp::Sub) {
    if (lhs_var && lhs_var->name == target && rhs_lit.has_value()) {
      return -*rhs_lit;
    }
  }
  return std::nullopt;
}

std::optional<long long> parse_branch_delta_switch_for(const StmtList& body,
                                                       const std::string& target) {
  if (body.empty()) {
    return 0;
  }
  if (body.size() > 2) {
    return std::nullopt;
  }
  const auto* assign = as_assign_stmt_for(body.front().get());
  if (!assign || !assign->target || assign->target->kind != Expr::Kind::Variable || !assign->value) {
    return std::nullopt;
  }
  const auto& assign_target = static_cast<const VariableExpr&>(*assign->target);
  if (assign_target.name != target) {
    return std::nullopt;
  }
  if (body.size() == 2 && body[1]->kind != Stmt::Kind::Break) {
    return std::nullopt;
  }
  const auto* binary = as_binary_expr_for(assign->value.get());
  if (!binary) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr_for(binary->left.get());
  const auto* rhs_var = as_variable_expr_for(binary->right.get());
  const auto lhs_lit = extract_int_literal_expr_for(binary->left.get());
  const auto rhs_lit = extract_int_literal_expr_for(binary->right.get());
  if (binary->op == BinaryOp::Add) {
    if (lhs_var && lhs_var->name == target && rhs_lit.has_value()) {
      return *rhs_lit;
    }
    if (rhs_var && rhs_var->name == target && lhs_lit.has_value()) {
      return *lhs_lit;
    }
    return std::nullopt;
  }
  if (binary->op == BinaryOp::Sub) {
    if (lhs_var && lhs_var->name == target && rhs_lit.has_value()) {
      return -*rhs_lit;
    }
  }
  return std::nullopt;
}

struct FastModuloDispatchPlanFor {
  std::string selector_variable;
  long long mod_base = 0;
  std::string target_variable;
  std::vector<long long> match_values;
  std::vector<long long> deltas;
  long long else_delta = 0;
  bool has_dense_table = false;
  std::vector<long long> dense_table;
};

std::optional<FastModuloDispatchPlanFor> build_fast_mod_dispatch_plan_for(
    const ForStmt& for_stmt) {
  if (for_stmt.body.size() != 2) {
    return std::nullopt;
  }
  const auto* selector_assign = as_assign_stmt_for(for_stmt.body[0].get());
  const auto* if_stmt = as_if_stmt_for(for_stmt.body[1].get());
  const auto* switch_stmt = as_switch_stmt_for(for_stmt.body[1].get());
  if (!selector_assign || !selector_assign->target || !selector_assign->value ||
      selector_assign->target->kind != Expr::Kind::Variable) {
    return std::nullopt;
  }
  if (!if_stmt && !switch_stmt) {
    return std::nullopt;
  }

  const auto& selector_var = static_cast<const VariableExpr&>(*selector_assign->target).name;
  const auto* selector_binary = as_binary_expr_for(selector_assign->value.get());
  if (!selector_binary || selector_binary->op != BinaryOp::Mod) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr_for(selector_binary->left.get());
  if (!lhs_var || lhs_var->name != for_stmt.name) {
    return std::nullopt;
  }
  const auto mod_base = extract_int_literal_expr_for(selector_binary->right.get());
  if (!mod_base.has_value() || *mod_base == 0) {
    return std::nullopt;
  }

  FastModuloDispatchPlanFor plan;
  plan.selector_variable = selector_var;
  plan.mod_base = *mod_base;

  if (if_stmt) {
    std::string cond_var;
    long long cond_lit = 0;
    if (!parse_eq_var_literal_for(if_stmt->condition.get(), cond_var, cond_lit) ||
        cond_var != selector_var) {
      return std::nullopt;
    }
    plan.match_values.push_back(cond_lit);

    for (const auto& branch : if_stmt->elif_branches) {
      std::string branch_var;
      long long branch_lit = 0;
      if (!parse_eq_var_literal_for(branch.first.get(), branch_var, branch_lit) ||
          branch_var != selector_var) {
        return std::nullopt;
      }
      plan.match_values.push_back(branch_lit);
    }

    if (if_stmt->then_body.size() != 1 || if_stmt->else_body.size() != 1) {
      return std::nullopt;
    }
    const auto* then_assign = as_assign_stmt_for(if_stmt->then_body.front().get());
    if (!then_assign || !then_assign->target || then_assign->target->kind != Expr::Kind::Variable) {
      return std::nullopt;
    }
    plan.target_variable = static_cast<const VariableExpr&>(*then_assign->target).name;

    const auto then_delta = parse_branch_delta_for(if_stmt->then_body, plan.target_variable);
    if (!then_delta.has_value()) {
      return std::nullopt;
    }
    plan.deltas.push_back(*then_delta);

    for (const auto& branch : if_stmt->elif_branches) {
      const auto delta = parse_branch_delta_for(branch.second, plan.target_variable);
      if (!delta.has_value()) {
        return std::nullopt;
      }
      plan.deltas.push_back(*delta);
    }
    const auto else_delta = parse_branch_delta_for(if_stmt->else_body, plan.target_variable);
    if (!else_delta.has_value()) {
      return std::nullopt;
    }
    plan.else_delta = *else_delta;
  } else {
    const auto* selector_expr = as_variable_expr_for(switch_stmt->selector.get());
    if (!selector_expr || selector_expr->name != selector_var || switch_stmt->cases.empty()) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < switch_stmt->cases.size(); ++i) {
      const auto& switch_case = switch_stmt->cases[i];
      const auto case_lit = extract_int_literal_expr_for(switch_case.match.get());
      if (!case_lit.has_value()) {
        return std::nullopt;
      }
      if (i == 0) {
        const auto* first_assign = switch_case.body.empty()
                                       ? nullptr
                                       : as_assign_stmt_for(switch_case.body.front().get());
        if (!first_assign || !first_assign->target ||
            first_assign->target->kind != Expr::Kind::Variable) {
          return std::nullopt;
        }
        plan.target_variable = static_cast<const VariableExpr&>(*first_assign->target).name;
      }
      const auto delta = parse_branch_delta_switch_for(switch_case.body, plan.target_variable);
      if (!delta.has_value()) {
        return std::nullopt;
      }
      plan.match_values.push_back(*case_lit);
      plan.deltas.push_back(*delta);
    }
    if (!switch_stmt->default_body.empty()) {
      const auto else_delta = parse_branch_delta_switch_for(switch_stmt->default_body, plan.target_variable);
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

std::uint64_t for_stmt_mod_dispatch_fingerprint(const ForStmt& for_stmt) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&](std::uintptr_t value) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
  };

  mix(reinterpret_cast<std::uintptr_t>(&for_stmt));
  mix(reinterpret_cast<std::uintptr_t>(for_stmt.iterable.get()));
  mix(static_cast<std::uintptr_t>(for_stmt.body.size()));
  for (const auto& body_stmt : for_stmt.body) {
    mix(reinterpret_cast<std::uintptr_t>(body_stmt.get()));
  }
  mix(static_cast<std::uintptr_t>(for_stmt.is_async ? 1 : 0));
  return hash;
}

const std::optional<FastModuloDispatchPlanFor>& cached_fast_mod_dispatch_plan_for(
    const ForStmt& for_stmt) {
  struct CacheEntry {
    std::uint64_t fingerprint = 0;
    std::optional<FastModuloDispatchPlanFor> plan;
  };
  static thread_local std::unordered_map<const ForStmt*, CacheEntry> cache;
  if (cache.size() > 8192U) {
    cache.clear();
  }

  auto& entry = cache[&for_stmt];
  const auto fingerprint = for_stmt_mod_dispatch_fingerprint(for_stmt);
  if (entry.fingerprint != fingerprint) {
    entry.plan = build_fast_mod_dispatch_plan_for(for_stmt);
    entry.fingerprint = fingerprint;
  }
  return entry.plan;
}

bool try_execute_fast_mod_dispatch_for_range(const ForStmt& for_stmt,
                                             const std::shared_ptr<Environment>& env,
                                             const RangeSpec& range_spec, Value& result) {
  const bool debug = std::getenv("SPARK_DEBUG_MOD_DISPATCH_FOR") != nullptr;
  auto fail = [&](const char* reason) -> bool {
    if (debug) {
      std::fprintf(stderr, "[mod-dispatch-for] %s\n", reason);
    }
    return false;
  };
  const auto& plan = cached_fast_mod_dispatch_plan_for(for_stmt);
  if (!plan.has_value()) {
    return fail("plan-build-failed");
  }
  auto* target = env->get_ptr(plan->target_variable);
  I128 target_i128 = 0;
  if (!target || !try_numeric_like_to_i128_for(*target, target_i128) ||
      target_i128 < static_cast<I128>(std::numeric_limits<long long>::min()) ||
      target_i128 > static_cast<I128>(std::numeric_limits<long long>::max())) {
    return fail("target-not-int-like");
  }
  long long target_value = static_cast<long long>(target_i128);

  const long long iterations = range_iteration_count(range_spec);
  if (iterations <= 0) {
    result = Value::nil();
    return true;
  }

  const __int128 last =
      static_cast<__int128>(range_spec.start) +
      static_cast<__int128>(range_spec.step) * static_cast<__int128>(iterations - 1);
  if (last < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
      last > static_cast<__int128>(std::numeric_limits<long long>::max())) {
    return fail("last-value-overflow");
  }
  const Value loop_last = Value::int_value_of(static_cast<long long>(last));
  if (!env->set(for_stmt.name, loop_last)) {
    env->define(for_stmt.name, loop_last);
  }

  if (plan->mod_base > 0 && range_spec.step == 1 && range_spec.start >= 0) {
    const long long m = plan->mod_base;
    const long long n = iterations;
    const long long q = n / m;
    const long long rem = n % m;
    const long long first = range_spec.start % m;

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

    const __int128 next =
        static_cast<__int128>(target_value) + static_cast<__int128>(total_delta);
    if (next < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
        next > static_cast<__int128>(std::numeric_limits<long long>::max())) {
      return fail("closed-form-overflow");
    }
    target_value = static_cast<long long>(next);
    if (!assign_i128_to_intlike_for(*target, static_cast<I128>(target_value))) {
      return fail("closed-form-assign-failed");
    }

    const long long selector_last = static_cast<long long>(last) % m;
    const Value selector_value = Value::int_value_of(selector_last);
    if (!env->set(plan->selector_variable, selector_value)) {
      env->define(plan->selector_variable, selector_value);
    }

    result = Value::nil();
    if (debug) {
      std::fprintf(stderr, "[mod-dispatch-for] closed-form-success\n");
    }
    return true;
  }

  long long iter_value = range_spec.start;
  long long last_selector = 0;
  bool has_selector = false;
  for (long long iter = 0; iter < iterations; ++iter) {
    const long long rem = iter_value % plan->mod_base;
    last_selector = rem;
    has_selector = true;
    long long delta = plan->else_delta;
    if (plan->has_dense_table) {
      if (rem >= 0 && rem < plan->mod_base) {
        delta = plan->dense_table[static_cast<std::size_t>(rem)];
      }
    } else {
      for (std::size_t i = 0; i < plan->match_values.size(); ++i) {
        if (rem == plan->match_values[i]) {
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
    iter_value += range_spec.step;
  }
  if (!assign_i128_to_intlike_for(*target, static_cast<I128>(target_value))) {
    return fail("iterative-assign-failed");
  }

  if (has_selector) {
    const Value selector_value = Value::int_value_of(last_selector);
    if (!env->set(plan->selector_variable, selector_value)) {
      env->define(plan->selector_variable, selector_value);
    }
  }

  result = Value::nil();
  if (debug) {
    std::fprintf(stderr, "[mod-dispatch-for] iterative-success\n");
  }
  return true;
}

struct AggregatedAssignPlan {
  enum class Mode {
    Recurrence,
    InvariantBinary,
  };

  std::string accumulator_name;
  Mode mode = Mode::Recurrence;
  BinaryOp op = BinaryOp::Add;
  const Expr* lhs_expr = nullptr;
  bool lhs_is_variable = false;
  std::string lhs_variable;
  const Expr* rhs_expr = nullptr;
  bool rhs_is_variable = false;
  std::string rhs_variable;
  bool recurrence_target_is_left = true;
};

std::optional<RangeSpec> parse_range_spec(const ForStmt& for_stmt, Interpreter& self,
                                          const std::shared_ptr<Environment>& env) {
  if (!for_stmt.iterable || for_stmt.iterable->kind != Expr::Kind::Call) {
    return std::nullopt;
  }
  const auto& call = static_cast<const CallExpr&>(*for_stmt.iterable);
  const auto* callee_var = as_variable_expr_for(call.callee.get());
  if (!callee_var || callee_var->name != "range") {
    return std::nullopt;
  }
  if (call.args.empty() || call.args.size() > 3) {
    return std::nullopt;
  }

  RangeSpec spec{};
  if (call.args.size() == 1) {
    spec.stop = value_to_int(self.evaluate(*call.args[0], env));
  } else {
    spec.start = value_to_int(self.evaluate(*call.args[0], env));
    spec.stop = value_to_int(self.evaluate(*call.args[1], env));
    if (call.args.size() == 3) {
      spec.step = value_to_int(self.evaluate(*call.args[2], env));
    }
  }
  if (spec.step == 0) {
    throw EvalException("range() step must not be zero");
  }
  return spec;
}

long long range_iteration_count(const RangeSpec& spec) {
  if (spec.step > 0) {
    if (spec.start >= spec.stop) {
      return 0;
    }
    const __int128 span = static_cast<__int128>(spec.stop) - static_cast<__int128>(spec.start);
    const __int128 step = static_cast<__int128>(spec.step);
    return static_cast<long long>((span + step - 1) / step);
  }
  if (spec.start <= spec.stop) {
    return 0;
  }
  const __int128 span = static_cast<__int128>(spec.start) - static_cast<__int128>(spec.stop);
  const __int128 step = static_cast<__int128>(-spec.step);
  return static_cast<long long>((span + step - 1) / step);
}

bool parse_for_numeric_assign(const AssignStmt& assign, AggregatedAssignPlan& plan) {
  const auto* target = as_variable_expr_for(assign.target.get());
  if (!target) {
    return false;
  }
  const auto* binary = as_binary_expr_for(assign.value.get());
  if (!binary || !is_numeric_arithmetic_op_for(binary->op)) {
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

  const bool recurrence_left = plan.lhs_is_variable && plan.lhs_variable == target->name;
  const bool recurrence_right = plan.rhs_is_variable && plan.rhs_variable == target->name;

  if (recurrence_left || recurrence_right) {
    plan.mode = AggregatedAssignPlan::Mode::Recurrence;
    plan.recurrence_target_is_left = recurrence_left;
    return true;
  }

  plan.mode = AggregatedAssignPlan::Mode::InvariantBinary;
  if ((plan.lhs_is_variable && plan.lhs_variable == target->name) ||
      (plan.rhs_is_variable && plan.rhs_variable == target->name)) {
    return false;
  }
  return true;
}

bool expr_depends_on_variable_for(const Expr* expr, const std::string& variable) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case Expr::Kind::Variable:
      return static_cast<const VariableExpr*>(expr)->name == variable;
    case Expr::Kind::Unary:
      return expr_depends_on_variable_for(static_cast<const UnaryExpr*>(expr)->operand.get(), variable);
    case Expr::Kind::Binary: {
      const auto* binary = static_cast<const BinaryExpr*>(expr);
      return expr_depends_on_variable_for(binary->left.get(), variable) ||
             expr_depends_on_variable_for(binary->right.get(), variable);
    }
    case Expr::Kind::Call: {
      const auto* call = static_cast<const CallExpr*>(expr);
      if (expr_depends_on_variable_for(call->callee.get(), variable)) {
        return true;
      }
      for (const auto& arg : call->args) {
        if (expr_depends_on_variable_for(arg.get(), variable)) {
          return true;
        }
      }
      return false;
    }
    case Expr::Kind::Attribute:
      return expr_depends_on_variable_for(static_cast<const AttributeExpr*>(expr)->target.get(), variable);
    case Expr::Kind::Index: {
      const auto* index = static_cast<const IndexExpr*>(expr);
      if (expr_depends_on_variable_for(index->target.get(), variable)) {
        return true;
      }
      for (const auto& item : index->indices) {
        if (item.is_slice) {
          if (expr_depends_on_variable_for(item.slice_start.get(), variable) ||
              expr_depends_on_variable_for(item.slice_stop.get(), variable) ||
              expr_depends_on_variable_for(item.slice_step.get(), variable)) {
            return true;
          }
        } else if (expr_depends_on_variable_for(item.index.get(), variable)) {
          return true;
        }
      }
      return false;
    }
    case Expr::Kind::List: {
      const auto* list = static_cast<const ListExpr*>(expr);
      for (const auto& item : list->elements) {
        if (expr_depends_on_variable_for(item.get(), variable)) {
          return true;
        }
      }
      return false;
    }
    case Expr::Kind::Number:
    case Expr::Kind::String:
    case Expr::Kind::Bool:
      return false;
  }
  return true;
}

bool expr_is_side_effect_free_for(const Expr* expr) {
  if (!expr) {
    return true;
  }
  switch (expr->kind) {
    case Expr::Kind::Number:
    case Expr::Kind::String:
    case Expr::Kind::Bool:
    case Expr::Kind::Variable:
      return true;
    case Expr::Kind::Unary:
      return expr_is_side_effect_free_for(static_cast<const UnaryExpr*>(expr)->operand.get());
    case Expr::Kind::Binary: {
      const auto* binary = static_cast<const BinaryExpr*>(expr);
      return expr_is_side_effect_free_for(binary->left.get()) &&
             expr_is_side_effect_free_for(binary->right.get());
    }
    case Expr::Kind::List: {
      const auto* list = static_cast<const ListExpr*>(expr);
      for (const auto& item : list->elements) {
        if (!expr_is_side_effect_free_for(item.get())) {
          return false;
        }
      }
      return true;
    }
    case Expr::Kind::Call:
    case Expr::Kind::Attribute:
    case Expr::Kind::Index:
      return false;
  }
  return false;
}

bool collect_aggregate_plans_from_block(const StmtList& body, const std::string& loop_variable,
                                        Interpreter& self, const std::shared_ptr<Environment>& env,
                                        std::vector<AggregatedAssignPlan>& out);

bool collect_aggregate_plan_from_stmt(const Stmt& stmt, const std::string& loop_variable,
                                      Interpreter& self, const std::shared_ptr<Environment>& env,
                                      std::vector<AggregatedAssignPlan>& out) {
  if (stmt.kind == Stmt::Kind::Assign) {
    const auto& assign = static_cast<const AssignStmt&>(stmt);
    AggregatedAssignPlan plan{};
    if (!parse_for_numeric_assign(assign, plan)) {
      return false;
    }
    out.push_back(std::move(plan));
    return true;
  }

  if (stmt.kind == Stmt::Kind::If) {
    const auto& if_stmt = static_cast<const IfStmt&>(stmt);
    if (!if_stmt.elif_branches.empty()) {
      return false;
    }
    if (!expr_is_side_effect_free_for(if_stmt.condition.get()) ||
        expr_depends_on_variable_for(if_stmt.condition.get(), loop_variable)) {
      return false;
    }
    const bool cond = self.truthy(self.evaluate(*if_stmt.condition, env));
    const auto& branch = cond ? if_stmt.then_body : if_stmt.else_body;
    return collect_aggregate_plans_from_block(branch, loop_variable, self, env, out);
  }

  return false;
}

bool collect_aggregate_plans_from_block(const StmtList& body, const std::string& loop_variable,
                                        Interpreter& self, const std::shared_ptr<Environment>& env,
                                        std::vector<AggregatedAssignPlan>& out) {
  for (const auto& stmt : body) {
    if (!collect_aggregate_plan_from_stmt(*stmt, loop_variable, self, env, out)) {
      return false;
    }
  }
  return true;
}

bool try_execute_fast_numeric_for_range(const ForStmt& for_stmt, Interpreter& self,
                                        const std::shared_ptr<Environment>& env,
                                        const RangeSpec& range_spec, Value& result) {
  if (for_stmt.body.empty()) {
    return true;
  }

  std::vector<AggregatedAssignPlan> plans;
  if (!collect_aggregate_plans_from_block(for_stmt.body, for_stmt.name, self, env, plans) ||
      plans.empty()) {
    return false;
  }

  const long long iterations = range_iteration_count(range_spec);
  if (iterations <= 0) {
    return true;
  }

  std::unordered_set<std::string> accumulators;
  accumulators.reserve(plans.size());
  for (const auto& plan : plans) {
    if (plan.accumulator_name == for_stmt.name) {
      return false;
    }
    if (!accumulators.insert(plan.accumulator_name).second) {
      return false;
    }
  }

  std::vector<Value*> accumulator_ptrs;
  accumulator_ptrs.reserve(plans.size());
  std::vector<Value> lhs_values;
  lhs_values.reserve(plans.size());
  std::vector<Value> rhs_values;
  rhs_values.reserve(plans.size());
  std::vector<bool> recurrence_other_is_loop_var;
  recurrence_other_is_loop_var.reserve(plans.size());
  for (const auto& plan : plans) {
    auto* accumulator_ptr = env->get_ptr(plan.accumulator_name);
    if (!accumulator_ptr || !is_numeric_kind(*accumulator_ptr)) {
      return false;
    }

    if (plan.mode == AggregatedAssignPlan::Mode::Recurrence) {
      const bool target_is_left = plan.recurrence_target_is_left;
      const bool other_is_variable = target_is_left ? plan.rhs_is_variable : plan.lhs_is_variable;
      const std::string& other_variable = target_is_left ? plan.rhs_variable : plan.lhs_variable;
      if (other_is_variable &&
          (other_variable == plan.accumulator_name || accumulators.count(other_variable) != 0U)) {
        return false;
      }
    } else {
      if ((plan.lhs_is_variable &&
           (plan.lhs_variable == for_stmt.name || accumulators.count(plan.lhs_variable) != 0U)) ||
          (plan.rhs_is_variable &&
           (plan.rhs_variable == for_stmt.name || accumulators.count(plan.rhs_variable) != 0U))) {
        return false;
      }
    }

    Value lhs = Value::nil();
    Value rhs = Value::nil();
    bool other_is_loop_var = false;
    if (plan.mode == AggregatedAssignPlan::Mode::InvariantBinary) {
      if (plan.lhs_is_variable) {
        auto* lhs_ptr = env->get_ptr(plan.lhs_variable);
        if (!lhs_ptr || !is_numeric_kind(*lhs_ptr)) {
          return false;
        }
        lhs = *lhs_ptr;
      } else {
        lhs = self.evaluate(*plan.lhs_expr, env);
        if (!is_numeric_kind(lhs)) {
          return false;
        }
      }

      if (plan.rhs_is_variable) {
        auto* rhs_ptr = env->get_ptr(plan.rhs_variable);
        if (!rhs_ptr || !is_numeric_kind(*rhs_ptr)) {
          return false;
        }
        rhs = *rhs_ptr;
      } else {
        rhs = self.evaluate(*plan.rhs_expr, env);
        if (!is_numeric_kind(rhs)) {
          return false;
        }
      }
    } else {
      const bool target_is_left = plan.recurrence_target_is_left;
      const bool other_is_variable = target_is_left ? plan.rhs_is_variable : plan.lhs_is_variable;
      const std::string& other_variable = target_is_left ? plan.rhs_variable : plan.lhs_variable;
      const Expr* other_expr = target_is_left ? plan.rhs_expr : plan.lhs_expr;
      if (other_is_variable) {
        if (other_variable == for_stmt.name) {
          other_is_loop_var = true;
        } else {
          auto* other_ptr = env->get_ptr(other_variable);
          if (!other_ptr || !is_numeric_kind(*other_ptr)) {
            return false;
          }
          if (target_is_left) {
            rhs = *other_ptr;
          } else {
            lhs = *other_ptr;
          }
        }
      } else {
        Value other = self.evaluate(*other_expr, env);
        if (!is_numeric_kind(other)) {
          return false;
        }
        if (target_is_left) {
          rhs = std::move(other);
        } else {
          lhs = std::move(other);
        }
      }
    }
    accumulator_ptrs.push_back(accumulator_ptr);
    lhs_values.push_back(std::move(lhs));
    rhs_values.push_back(std::move(rhs));
    recurrence_other_is_loop_var.push_back(other_is_loop_var);
  }

  // Keep for-loop post-state compatible: bind loop variable to last produced value.
  const __int128 last =
      static_cast<__int128>(range_spec.start) +
      static_cast<__int128>(range_spec.step) * static_cast<__int128>(iterations - 1);
  if (last < static_cast<__int128>(std::numeric_limits<long long>::min()) ||
      last > static_cast<__int128>(std::numeric_limits<long long>::max())) {
    return false;
  }
  const Value loop_last = Value::int_value_of(static_cast<long long>(last));
  if (!env->set(for_stmt.name, loop_last)) {
    env->define(for_stmt.name, loop_last);
  }

  for (std::size_t i = 0; i < plans.size(); ++i) {
    if (plans[i].mode == AggregatedAssignPlan::Mode::InvariantBinary) {
      if (!is_numeric_kind(lhs_values[i]) || !is_numeric_kind(rhs_values[i])) {
        return false;
      }
      *accumulator_ptrs[i] = eval_numeric_binary_value(plans[i].op, lhs_values[i], rhs_values[i]);
    } else {
      const bool target_is_left = plans[i].recurrence_target_is_left;
      const bool other_is_loop_var = recurrence_other_is_loop_var[i];
      if (!other_is_loop_var && target_is_left) {
        if (!eval_numeric_repeat_inplace(plans[i].op, *accumulator_ptrs[i], rhs_values[i], iterations)) {
          return false;
        }
        continue;
      }
      if (!other_is_loop_var && !target_is_left && is_commutative_numeric_op_for(plans[i].op)) {
        if (!eval_numeric_repeat_inplace(plans[i].op, *accumulator_ptrs[i], lhs_values[i], iterations)) {
          return false;
        }
        continue;
      }

      long long iter_value = range_spec.start;
      Value loop_operand = Value::int_value_of(iter_value);
      for (long long iter = 0; iter < iterations; ++iter) {
        if (other_is_loop_var) {
          loop_operand.int_value = iter_value;
        }

        if (target_is_left) {
          const Value& rhs = other_is_loop_var ? loop_operand : rhs_values[i];
          if (!eval_numeric_binary_value_inplace(plans[i].op, *accumulator_ptrs[i], rhs,
                                                 *accumulator_ptrs[i])) {
            return false;
          }
        } else {
          const Value& lhs = other_is_loop_var ? loop_operand : lhs_values[i];
          Value next = eval_numeric_binary_value(plans[i].op, lhs, *accumulator_ptrs[i]);
          if (!copy_numeric_value_inplace(*accumulator_ptrs[i], next)) {
            *accumulator_ptrs[i] = std::move(next);
          }
        }
        iter_value += range_spec.step;
      }
    }
  }
  result = Value::nil();
  return true;
}

}  // namespace

namespace {

bool stmt_has_outer_loop_control_for(const Stmt& stmt);

bool block_has_outer_loop_control_for(const StmtList& body) {
  for (const auto& item : body) {
    if (item && stmt_has_outer_loop_control_for(*item)) {
      return true;
    }
  }
  return false;
}

bool stmt_has_outer_loop_control_for(const Stmt& stmt) {
  switch (stmt.kind) {
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
      return true;
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(stmt);
      if (block_has_outer_loop_control_for(if_stmt.then_body) ||
          block_has_outer_loop_control_for(if_stmt.else_body)) {
        return true;
      }
      for (const auto& branch : if_stmt.elif_branches) {
        if (block_has_outer_loop_control_for(branch.second)) {
          return true;
        }
      }
      return false;
    }
    case Stmt::Kind::Switch: {
      const auto& switch_stmt = static_cast<const SwitchStmt&>(stmt);
      for (const auto& switch_case : switch_stmt.cases) {
        if (block_has_outer_loop_control_for(switch_case.body)) {
          return true;
        }
      }
      return block_has_outer_loop_control_for(switch_stmt.default_body);
    }
    case Stmt::Kind::TryCatch: {
      const auto& try_stmt = static_cast<const TryCatchStmt&>(stmt);
      return block_has_outer_loop_control_for(try_stmt.try_body) ||
             block_has_outer_loop_control_for(try_stmt.catch_body);
    }
    case Stmt::Kind::WithTaskGroup: {
      const auto& with_stmt = static_cast<const WithTaskGroupStmt&>(stmt);
      return block_has_outer_loop_control_for(with_stmt.body);
    }
    case Stmt::Kind::For:
    case Stmt::Kind::While:
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

Value execute_case_for(const ForStmt& for_stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env) {
  const bool has_outer_loop_control = block_has_outer_loop_control_for(for_stmt.body);

  std::optional<RangeSpec> range_spec = std::nullopt;
  if (!for_stmt.is_async && !has_outer_loop_control &&
      env_bool_enabled_for("SPARK_FOR_FAST_RANGE", true)) {
    range_spec = parse_range_spec(for_stmt, self, env);
    if (range_spec.has_value() &&
        env_bool_enabled_for("SPARK_FOR_FAST_NUMERIC", true)) {
      Value fast_result = Value::nil();
      if (try_execute_fast_mod_dispatch_for_range(for_stmt, env, *range_spec, fast_result)) {
        return fast_result;
      }
      if (try_execute_fast_numeric_for_range(for_stmt, self, env, *range_spec, fast_result)) {
        return fast_result;
      }
    }
  }

  auto sequence = range_spec.has_value() ? Value::nil() : self.evaluate(*for_stmt.iterable, env);
  const auto& body = for_stmt.body;
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

  Value* loop_slot = nullptr;
  auto bind_loop_var = [&](const Value& value) mutable {
    if (!loop_slot) {
      if (!env->set(for_stmt.name, value)) {
        env->define(for_stmt.name, value);
      }
      loop_slot = env->get_ptr(for_stmt.name);
      if (!loop_slot) {
        throw EvalException("failed to bind loop variable");
      }
      return;
    }
    if (!copy_numeric_value_inplace(*loop_slot, value)) {
      *loop_slot = value;
    }
  };
  auto bind_loop_var_int = [&](long long value) mutable {
    if (!loop_slot) {
      const Value loop_value = Value::int_value_of(value);
      if (!env->set(for_stmt.name, loop_value)) {
        env->define(for_stmt.name, loop_value);
      }
      loop_slot = env->get_ptr(for_stmt.name);
      if (!loop_slot) {
        throw EvalException("failed to bind loop variable");
      }
      return;
    }
    if (loop_slot->kind == Value::Kind::Int) {
      loop_slot->int_value = value;
      return;
    }
    Value next = Value::int_value_of(value);
    if (!copy_numeric_value_inplace(*loop_slot, next)) {
      *loop_slot = std::move(next);
    }
  };

  bool break_loop = false;
  const auto execute_body = [&](Value& result) -> bool {
    if (body.empty()) {
      return true;
    }
    if (!has_outer_loop_control) {
      if (single_body_thunk.has_value()) {
        result = execute_stmt_thunk(*single_body_thunk, self, env);
        return true;
      }
      for (const auto& thunk : body_thunks) {
        result = execute_stmt_thunk(thunk, self, env);
      }
      return true;
    }
    try {
      if (single_body_thunk.has_value()) {
        result = execute_stmt_thunk(*single_body_thunk, self, env);
        return true;
      }
      for (const auto& thunk : body_thunks) {
        result = execute_stmt_thunk(thunk, self, env);
      }
      return true;
    } catch (const Interpreter::ContinueSignal&) {
      return true;
    } catch (const Interpreter::BreakSignal&) {
      break_loop = true;
      return false;
    }
  };

  if (!for_stmt.is_async && range_spec.has_value()) {
    Value result = Value::nil();
    if (range_spec->step > 0) {
      for (long long i = range_spec->start; i < range_spec->stop; i += range_spec->step) {
        bind_loop_var_int(i);
        (void)execute_body(result);
        if (break_loop) {
          break;
        }
      }
    } else {
      for (long long i = range_spec->start; i > range_spec->stop; i += range_spec->step) {
        bind_loop_var_int(i);
        (void)execute_body(result);
        if (break_loop) {
          break;
        }
      }
    }
    return result;
  }

  if (for_stmt.is_async && sequence.kind == Value::Kind::Channel) {
    Value result = Value::nil();
    while (true) {
      const auto item = stream_next_value(sequence);
      if (item.kind == Value::Kind::Nil) {
        break;
      }
      bind_loop_var(item);
      (void)execute_body(result);
      if (break_loop) {
        break;
      }
    }
    return result;
  }

  if (sequence.kind != Value::Kind::List) {
    if (sequence.kind != Value::Kind::Matrix) {
      throw EvalException(for_stmt.is_async
                              ? "async for loop requires channel/list/matrix iterable"
                              : "for loop requires list or matrix iterable");
    }
  }
  Value result = Value::nil();
  if (sequence.kind == Value::Kind::List) {
    for (const auto& item : sequence.list_value) {
      bind_loop_var(item);
      (void)execute_body(result);
      if (break_loop) {
        break;
      }
    }
    return result;
  }

  const auto* matrix = sequence.matrix_value.get();
  const auto rows = matrix ? matrix->rows : 0;
  const auto cols = matrix ? matrix->cols : 0;
  if (matrix && cols == 1) {
    // Single-column matrices can stream scalar cells directly.
    for (std::size_t row = 0; row < rows; ++row) {
      bind_loop_var(matrix->data[row]);
      (void)execute_body(result);
      if (break_loop) {
        break;
      }
    }
    return result;
  }

  // Multi-column matrix iteration keeps Python-like row-list semantics, but
  // reuses a row buffer to avoid per-iteration list allocations.
  Value row_value = Value::list_value_of(std::vector<Value>(cols));
  for (std::size_t row = 0; row < rows; ++row) {
    const auto base = row * cols;
    for (std::size_t col = 0; col < cols; ++col) {
      row_value.list_value[col] = matrix->data[base + col];
    }
    bind_loop_var(row_value);
    (void)execute_body(result);
    if (break_loop) {
      break;
    }
  }
  return result;
}

}  // namespace spark
