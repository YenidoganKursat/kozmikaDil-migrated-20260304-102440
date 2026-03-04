#include "../internal_helpers.h"

#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace spark {

namespace {

const VariableExpr* as_variable_expr_if(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return static_cast<const VariableExpr*>(expr);
}

const NumberExpr* as_number_expr_if(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Number) {
    return nullptr;
  }
  return static_cast<const NumberExpr*>(expr);
}

const BinaryExpr* as_binary_expr_if(const Expr* expr) {
  if (!expr || expr->kind != Expr::Kind::Binary) {
    return nullptr;
  }
  return static_cast<const BinaryExpr*>(expr);
}

const AssignStmt* as_assign_stmt_if(const Stmt* stmt) {
  if (!stmt || stmt->kind != Stmt::Kind::Assign) {
    return nullptr;
  }
  return static_cast<const AssignStmt*>(stmt);
}

const VariableExpr* assign_target_variable_if(const AssignStmt& assign) {
  if (!assign.target || assign.target->kind != Expr::Kind::Variable) {
    return nullptr;
  }
  return static_cast<const VariableExpr*>(assign.target.get());
}

std::optional<long long> number_to_i64_if(const NumberExpr& number) {
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

struct FastIntIfConditionPlan {
  enum class OperandMode {
    Variable,
    Literal,
    IntExpr,
  };

  BinaryOp op = BinaryOp::Eq;
  OperandMode lhs_mode = OperandMode::Literal;
  std::string lhs_variable;
  long long lhs_literal = 0;
  const Expr* lhs_expr = nullptr;
  OperandMode rhs_mode = OperandMode::Literal;
  std::string rhs_variable;
  long long rhs_literal = 0;
  const Expr* rhs_expr = nullptr;
};

bool parse_int_numeric_constructor_call_if(const Expr* expr, const Expr*& out_arg) {
  const auto* call = dynamic_cast<const CallExpr*>(expr);
  if (!call || !call->callee || call->args.size() != 1 ||
      call->callee->kind != Expr::Kind::Variable) {
    return false;
  }
  const auto& callee = static_cast<const VariableExpr&>(*call->callee);
  try {
    const auto kind = numeric_kind_from_name(callee.name);
    if (!numeric_kind_is_int(kind)) {
      return false;
    }
  } catch (const EvalException&) {
    return false;
  }
  out_arg = call->args[0].get();
  return true;
}

bool expr_supports_fast_int_eval_if(const Expr* expr) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case Expr::Kind::Number: {
      const auto* number = static_cast<const NumberExpr*>(expr);
      return number_to_i64_if(*number).has_value();
    }
    case Expr::Kind::Variable:
      return true;
    case Expr::Kind::Unary: {
      const auto* unary = static_cast<const UnaryExpr*>(expr);
      return unary->op == UnaryOp::Neg &&
             expr_supports_fast_int_eval_if(unary->operand.get());
    }
    case Expr::Kind::Binary: {
      const auto* binary = static_cast<const BinaryExpr*>(expr);
      if (!binary->left || !binary->right) {
        return false;
      }
      if (binary->op != BinaryOp::Add && binary->op != BinaryOp::Sub &&
          binary->op != BinaryOp::Mul && binary->op != BinaryOp::Mod) {
        return false;
      }
      return expr_supports_fast_int_eval_if(binary->left.get()) &&
             expr_supports_fast_int_eval_if(binary->right.get());
    }
    case Expr::Kind::Call: {
      const Expr* ctor_arg = nullptr;
      if (!parse_int_numeric_constructor_call_if(expr, ctor_arg)) {
        return false;
      }
      return expr_supports_fast_int_eval_if(ctor_arg);
    }
    default:
      return false;
  }
}

bool try_eval_fast_int_expr_if(const Expr* expr, const std::shared_ptr<Environment>& env,
                               long long& out) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case Expr::Kind::Number: {
      const auto* number = static_cast<const NumberExpr*>(expr);
      const auto value = number_to_i64_if(*number);
      if (!value.has_value()) {
        return false;
      }
      out = *value;
      return true;
    }
    case Expr::Kind::Variable: {
      const auto& variable = static_cast<const VariableExpr&>(*expr);
      const auto* value = env->get_ptr(variable.name);
      if (!value || value->kind != Value::Kind::Int) {
        return false;
      }
      out = value->int_value;
      return true;
    }
    case Expr::Kind::Unary: {
      const auto* unary = static_cast<const UnaryExpr*>(expr);
      if (unary->op != UnaryOp::Neg) {
        return false;
      }
      long long operand = 0;
      if (!try_eval_fast_int_expr_if(unary->operand.get(), env, operand)) {
        return false;
      }
      out = -operand;
      return true;
    }
    case Expr::Kind::Binary: {
      const auto* binary = static_cast<const BinaryExpr*>(expr);
      long long lhs = 0;
      long long rhs = 0;
      if (!try_eval_fast_int_expr_if(binary->left.get(), env, lhs) ||
          !try_eval_fast_int_expr_if(binary->right.get(), env, rhs)) {
        return false;
      }
      switch (binary->op) {
        case BinaryOp::Add:
          out = lhs + rhs;
          return true;
        case BinaryOp::Sub:
          out = lhs - rhs;
          return true;
        case BinaryOp::Mul:
          out = lhs * rhs;
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
    case Expr::Kind::Call: {
      const Expr* ctor_arg = nullptr;
      if (!parse_int_numeric_constructor_call_if(expr, ctor_arg)) {
        return false;
      }
      return try_eval_fast_int_expr_if(ctor_arg, env, out);
    }
    default:
      return false;
  }
}

std::optional<FastIntIfConditionPlan> build_fast_int_if_condition_plan(const Expr* condition) {
  const auto* binary = as_binary_expr_if(condition);
  if (!binary) {
    return std::nullopt;
  }
  switch (binary->op) {
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Lte:
    case BinaryOp::Gt:
    case BinaryOp::Gte:
      break;
    default:
      return std::nullopt;
  }

  FastIntIfConditionPlan plan;
  plan.op = binary->op;

  if (const auto* lhs_var = as_variable_expr_if(binary->left.get())) {
    plan.lhs_mode = FastIntIfConditionPlan::OperandMode::Variable;
    plan.lhs_variable = lhs_var->name;
  } else if (const auto* lhs_number = as_number_expr_if(binary->left.get())) {
    const auto value = number_to_i64_if(*lhs_number);
    if (!value.has_value()) {
      return std::nullopt;
    }
    plan.lhs_mode = FastIntIfConditionPlan::OperandMode::Literal;
    plan.lhs_literal = *value;
  } else if (expr_supports_fast_int_eval_if(binary->left.get())) {
    plan.lhs_mode = FastIntIfConditionPlan::OperandMode::IntExpr;
    plan.lhs_expr = binary->left.get();
  } else {
    return std::nullopt;
  }

  if (const auto* rhs_var = as_variable_expr_if(binary->right.get())) {
    plan.rhs_mode = FastIntIfConditionPlan::OperandMode::Variable;
    plan.rhs_variable = rhs_var->name;
  } else if (const auto* rhs_number = as_number_expr_if(binary->right.get())) {
    const auto value = number_to_i64_if(*rhs_number);
    if (!value.has_value()) {
      return std::nullopt;
    }
    plan.rhs_mode = FastIntIfConditionPlan::OperandMode::Literal;
    plan.rhs_literal = *value;
  } else if (expr_supports_fast_int_eval_if(binary->right.get())) {
    plan.rhs_mode = FastIntIfConditionPlan::OperandMode::IntExpr;
    plan.rhs_expr = binary->right.get();
  } else {
    return std::nullopt;
  }

  return plan;
}

bool eval_fast_int_if_condition(const FastIntIfConditionPlan& plan,
                                const std::shared_ptr<Environment>& env, bool& ok) {
  ok = false;
  long long lhs = plan.lhs_literal;
  long long rhs = plan.rhs_literal;

  switch (plan.lhs_mode) {
    case FastIntIfConditionPlan::OperandMode::Variable: {
      const auto* value = env->get_ptr(plan.lhs_variable);
      if (!value || value->kind != Value::Kind::Int) {
        return false;
      }
      lhs = value->int_value;
      break;
    }
    case FastIntIfConditionPlan::OperandMode::Literal:
      break;
    case FastIntIfConditionPlan::OperandMode::IntExpr:
      if (!try_eval_fast_int_expr_if(plan.lhs_expr, env, lhs)) {
        return false;
      }
      break;
  }
  switch (plan.rhs_mode) {
    case FastIntIfConditionPlan::OperandMode::Variable: {
      const auto* value = env->get_ptr(plan.rhs_variable);
      if (!value || value->kind != Value::Kind::Int) {
        return false;
      }
      rhs = value->int_value;
      break;
    }
    case FastIntIfConditionPlan::OperandMode::Literal:
      break;
    case FastIntIfConditionPlan::OperandMode::IntExpr:
      if (!try_eval_fast_int_expr_if(plan.rhs_expr, env, rhs)) {
        return false;
      }
      break;
  }

  ok = true;
  switch (plan.op) {
    case BinaryOp::Eq:
      return lhs == rhs;
    case BinaryOp::Ne:
      return lhs != rhs;
    case BinaryOp::Lt:
      return lhs < rhs;
    case BinaryOp::Lte:
      return lhs <= rhs;
    case BinaryOp::Gt:
      return lhs > rhs;
    case BinaryOp::Gte:
      return lhs >= rhs;
    default:
      ok = false;
      return false;
  }
}

struct FastIfPlanCache {
  std::uint64_t fingerprint = 0;
  std::optional<FastIntIfConditionPlan> main_plan;
  std::vector<std::optional<FastIntIfConditionPlan>> elif_plans;
  struct FastIntEqChainPlan {
    std::string variable;
    std::vector<long long> match_values;
  };
  std::optional<FastIntEqChainPlan> eq_chain_plan;
  struct FastIntEqChainDeltaPlan {
    std::string target_variable;
    std::vector<long long> deltas;
  };
  std::optional<FastIntEqChainDeltaPlan> eq_chain_delta_plan;
};

std::optional<long long> extract_int_literal_expr_if(const Expr* expr) {
  if (!expr) {
    return std::nullopt;
  }
  if (const auto* number = as_number_expr_if(expr)) {
    return number_to_i64_if(*number);
  }
  if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
    if (unary->op != UnaryOp::Neg || !unary->operand) {
      return std::nullopt;
    }
    const auto inner = extract_int_literal_expr_if(unary->operand.get());
    if (!inner.has_value()) {
      return std::nullopt;
    }
    return -*inner;
  }
  const Expr* ctor_arg = nullptr;
  if (parse_int_numeric_constructor_call_if(expr, ctor_arg)) {
    return extract_int_literal_expr_if(ctor_arg);
  }
  return std::nullopt;
}

bool parse_eq_var_literal_if(const Expr* condition, std::string& out_variable, long long& out_literal) {
  const auto* binary = as_binary_expr_if(condition);
  if (!binary || binary->op != BinaryOp::Eq) {
    return false;
  }
  if (const auto* lhs_var = as_variable_expr_if(binary->left.get())) {
    const auto rhs_lit = extract_int_literal_expr_if(binary->right.get());
    if (!rhs_lit.has_value()) {
      return false;
    }
    out_variable = lhs_var->name;
    out_literal = *rhs_lit;
    return true;
  }
  if (const auto* rhs_var = as_variable_expr_if(binary->right.get())) {
    const auto lhs_lit = extract_int_literal_expr_if(binary->left.get());
    if (!lhs_lit.has_value()) {
      return false;
    }
    out_variable = rhs_var->name;
    out_literal = *lhs_lit;
    return true;
  }
  return false;
}

std::optional<FastIfPlanCache::FastIntEqChainPlan> build_fast_int_eq_chain_plan(
    const IfStmt& if_stmt) {
  std::string variable;
  long long literal = 0;
  if (!parse_eq_var_literal_if(if_stmt.condition.get(), variable, literal)) {
    return std::nullopt;
  }
  FastIfPlanCache::FastIntEqChainPlan plan;
  plan.variable = std::move(variable);
  plan.match_values.push_back(literal);

  for (const auto& branch : if_stmt.elif_branches) {
    std::string branch_variable;
    long long branch_literal = 0;
    if (!parse_eq_var_literal_if(branch.first.get(), branch_variable, branch_literal)) {
      return std::nullopt;
    }
    if (branch_variable != plan.variable) {
      return std::nullopt;
    }
    plan.match_values.push_back(branch_literal);
  }
  return plan;
}

std::optional<long long> parse_branch_delta_if(const StmtList& body, const std::string& target) {
  if (body.size() != 1) {
    return std::nullopt;
  }
  const auto* assign = as_assign_stmt_if(body.front().get());
  const auto* assign_target = assign ? assign_target_variable_if(*assign) : nullptr;
  if (!assign || !assign_target || assign_target->name != target || !assign->value) {
    return std::nullopt;
  }
  const auto* binary = as_binary_expr_if(assign->value.get());
  if (!binary) {
    return std::nullopt;
  }
  const auto* lhs_var = as_variable_expr_if(binary->left.get());
  const auto* rhs_var = as_variable_expr_if(binary->right.get());
  const auto lhs_lit = extract_int_literal_expr_if(binary->left.get());
  const auto rhs_lit = extract_int_literal_expr_if(binary->right.get());

  switch (binary->op) {
    case BinaryOp::Add:
      if (lhs_var && lhs_var->name == target && rhs_lit.has_value()) {
        return *rhs_lit;
      }
      if (rhs_var && rhs_var->name == target && lhs_lit.has_value()) {
        return *lhs_lit;
      }
      return std::nullopt;
    case BinaryOp::Sub:
      if (lhs_var && lhs_var->name == target && rhs_lit.has_value()) {
        return -*rhs_lit;
      }
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

std::optional<FastIfPlanCache::FastIntEqChainDeltaPlan> build_fast_int_eq_chain_delta_plan(
    const IfStmt& if_stmt) {
  if (if_stmt.then_body.size() != 1) {
    return std::nullopt;
  }
  const auto* then_assign = as_assign_stmt_if(if_stmt.then_body.front().get());
  const auto* then_target = then_assign ? assign_target_variable_if(*then_assign) : nullptr;
  if (!then_assign || !then_target) {
    return std::nullopt;
  }

  FastIfPlanCache::FastIntEqChainDeltaPlan plan;
  plan.target_variable = then_target->name;
  plan.deltas.reserve(if_stmt.elif_branches.size() + 2);

  const auto then_delta = parse_branch_delta_if(if_stmt.then_body, plan.target_variable);
  if (!then_delta.has_value()) {
    return std::nullopt;
  }
  plan.deltas.push_back(*then_delta);

  for (const auto& branch : if_stmt.elif_branches) {
    const auto delta = parse_branch_delta_if(branch.second, plan.target_variable);
    if (!delta.has_value()) {
      return std::nullopt;
    }
    plan.deltas.push_back(*delta);
  }

  const auto else_delta = parse_branch_delta_if(if_stmt.else_body, plan.target_variable);
  if (!else_delta.has_value()) {
    return std::nullopt;
  }
  plan.deltas.push_back(*else_delta);
  return plan;
}

std::uint64_t if_stmt_fingerprint(const IfStmt& if_stmt) {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto mix = [&](std::uintptr_t value) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
  };

  mix(reinterpret_cast<std::uintptr_t>(&if_stmt));
  mix(reinterpret_cast<std::uintptr_t>(if_stmt.condition.get()));
  mix(static_cast<std::uintptr_t>(if_stmt.then_body.size()));
  for (const auto& stmt : if_stmt.then_body) {
    mix(reinterpret_cast<std::uintptr_t>(stmt.get()));
  }
  mix(static_cast<std::uintptr_t>(if_stmt.elif_branches.size()));
  for (const auto& branch : if_stmt.elif_branches) {
    mix(reinterpret_cast<std::uintptr_t>(branch.first.get()));
    mix(static_cast<std::uintptr_t>(branch.second.size()));
    for (const auto& stmt : branch.second) {
      mix(reinterpret_cast<std::uintptr_t>(stmt.get()));
    }
  }
  mix(static_cast<std::uintptr_t>(if_stmt.else_body.size()));
  for (const auto& stmt : if_stmt.else_body) {
    mix(reinterpret_cast<std::uintptr_t>(stmt.get()));
  }
  return hash;
}

FastIfPlanCache& fast_if_plan_cache(const IfStmt& if_stmt) {
  static std::unordered_map<const IfStmt*, FastIfPlanCache> cache;
  if (cache.size() > 8192U) {
    cache.clear();
  }
  auto& entry = cache[&if_stmt];
  // Rebuild every access. AST pointer reuse across multiple program parses in a
  // single process can otherwise reuse stale branch plans.
  entry.main_plan = build_fast_int_if_condition_plan(if_stmt.condition.get());
  entry.elif_plans.clear();
  entry.elif_plans.reserve(if_stmt.elif_branches.size());
  for (const auto& branch : if_stmt.elif_branches) {
    entry.elif_plans.push_back(build_fast_int_if_condition_plan(branch.first.get()));
  }
  entry.eq_chain_plan = build_fast_int_eq_chain_plan(if_stmt);
  entry.eq_chain_delta_plan = build_fast_int_eq_chain_delta_plan(if_stmt);
  entry.fingerprint = if_stmt_fingerprint(if_stmt);
  return entry;
}

inline void execute_block_fast_if(const StmtList& body, Interpreter& self,
                                  const std::shared_ptr<Environment>& env) {
  for (const auto& child : body) {
    execute_stmt_fast(*child, self, env);
  }
}

}  // namespace

Value execute_case_if(const IfStmt& if_stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env) {
  auto& cache = fast_if_plan_cache(if_stmt);

  if (cache.eq_chain_plan.has_value()) {
    const auto& plan = *cache.eq_chain_plan;
    const auto* value = env->get_ptr(plan.variable);
    if (value && value->kind == Value::Kind::Int) {
      const auto current = value->int_value;
      const auto apply_eq_chain_delta = [&](std::size_t branch_index) -> bool {
        if (!cache.eq_chain_delta_plan.has_value()) {
          return false;
        }
        const auto& delta_plan = *cache.eq_chain_delta_plan;
        if (branch_index >= delta_plan.deltas.size()) {
          return false;
        }
        auto* target = env->get_ptr(delta_plan.target_variable);
        if (!target || target->kind != Value::Kind::Int) {
          return false;
        }
        long long next = 0;
        if (__builtin_add_overflow(target->int_value, delta_plan.deltas[branch_index], &next)) {
          return false;
        }
        target->int_value = next;
        return true;
      };

      if (!plan.match_values.empty() && current == plan.match_values[0]) {
        if (!apply_eq_chain_delta(0)) {
          execute_block_fast_if(if_stmt.then_body, self, env);
        }
        return Value::nil();
      }
      for (std::size_t idx = 1; idx < plan.match_values.size(); ++idx) {
        if (current == plan.match_values[idx]) {
          if (!apply_eq_chain_delta(idx) && idx - 1 < if_stmt.elif_branches.size()) {
            execute_block_fast_if(if_stmt.elif_branches[idx - 1].second, self, env);
          }
          return Value::nil();
        }
      }
      if (!apply_eq_chain_delta(plan.match_values.size())) {
        execute_block_fast_if(if_stmt.else_body, self, env);
      }
      return Value::nil();
    }
  }

  bool fast_ok = false;
  bool condition_true = false;
  if (cache.main_plan.has_value()) {
    condition_true = eval_fast_int_if_condition(*cache.main_plan, env, fast_ok);
  }
  if (!fast_ok) {
    condition_true = self.truthy(self.evaluate(*if_stmt.condition, env));
  }

  if (condition_true) {
    execute_block_fast_if(if_stmt.then_body, self, env);
    return Value::nil();
  }

  for (std::size_t idx = 0; idx < if_stmt.elif_branches.size(); ++idx) {
    const auto& branch = if_stmt.elif_branches[idx];
    bool branch_fast_ok = false;
    bool branch_true = false;
    if (idx < cache.elif_plans.size() && cache.elif_plans[idx].has_value()) {
      branch_true = eval_fast_int_if_condition(*cache.elif_plans[idx], env, branch_fast_ok);
    }
    if (!branch_fast_ok) {
      branch_true = self.truthy(self.evaluate(*branch.first, env));
    }
    if (branch_true) {
      execute_block_fast_if(branch.second, self, env);
      return Value::nil();
    }
  }
  execute_block_fast_if(if_stmt.else_body, self, env);
  return Value::nil();
}

}  // namespace spark
