#include "../internal_helpers.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace spark {

namespace {

bool env_bool_enabled_while(const char* name, bool fallback) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  const std::string text = value;
  if (text == "0" || text == "false" || text == "False" || text == "off" || text == "OFF" ||
      text == "no" || text == "NO") {
    return false;
  }
  return true;
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

bool is_literal_one(const Expr* expr) {
  const auto* number = as_number_expr(expr);
  if (!number) {
    return false;
  }
  const auto maybe = number_to_i64(*number);
  return maybe.has_value() && *maybe == 1;
}

bool is_index_increment_assign(const AssignStmt& assign, const std::string& index_name) {
  const auto* target = as_variable_expr(assign.target.get());
  if (!target || target->name != index_name) {
    return false;
  }
  const auto* binary = as_binary_expr(assign.value.get());
  if (!binary || binary->op != BinaryOp::Add) {
    return false;
  }
  const auto* lhs_var = as_variable_expr(binary->left.get());
  const auto* rhs_var = as_variable_expr(binary->right.get());
  const bool lhs_index_rhs_one =
      lhs_var && lhs_var->name == index_name && is_literal_one(binary->right.get());
  const bool rhs_index_lhs_one =
      rhs_var && rhs_var->name == index_name && is_literal_one(binary->left.get());
  return lhs_index_rhs_one || rhs_index_lhs_one;
}

bool parse_accumulator_assign(const AssignStmt& assign, std::string& accumulator_name,
                              BinaryOp& op, const Expr*& rhs_expr) {
  const auto* target = as_variable_expr(assign.target.get());
  if (!target) {
    return false;
  }
  const auto* binary = as_binary_expr(assign.value.get());
  if (!binary || !is_numeric_arithmetic_op_while(binary->op)) {
    return false;
  }
  const auto* lhs = as_variable_expr(binary->left.get());
  if (!lhs || lhs->name != target->name) {
    return false;
  }
  if (!binary->right || (binary->right->kind != Expr::Kind::Variable &&
                         binary->right->kind != Expr::Kind::Number)) {
    return false;
  }
  accumulator_name = target->name;
  op = binary->op;
  rhs_expr = binary->right.get();
  return true;
}

struct FastNumericWhilePlan {
  std::string index_name;
  std::string accumulator_name;
  BinaryOp op = BinaryOp::Add;
  const Expr* rhs_expr = nullptr;
  bool rhs_is_variable = false;
  std::string rhs_variable;
  bool increment_after_operation = true;
  bool limit_is_variable = false;
  long long limit_value = 0;
  std::string limit_variable;
};

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

  const bool first_is_increment = is_index_increment_assign(*first, plan.index_name);
  const bool second_is_increment = is_index_increment_assign(*second, plan.index_name);
  if (first_is_increment == second_is_increment) {
    return std::nullopt;
  }

  const auto* arithmetic = first_is_increment ? second : first;
  if (!parse_accumulator_assign(*arithmetic, plan.accumulator_name, plan.op, plan.rhs_expr)) {
    return std::nullopt;
  }
  if (plan.accumulator_name == plan.index_name) {
    return std::nullopt;
  }

  plan.increment_after_operation = !first_is_increment;
  plan.rhs_is_variable = plan.rhs_expr->kind == Expr::Kind::Variable;
  if (plan.rhs_is_variable) {
    plan.rhs_variable = static_cast<const VariableExpr*>(plan.rhs_expr)->name;
  }

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

void increment_index_checked(Value& index) {
  if (index.kind != Value::Kind::Int) {
    throw EvalException("fast while path expects integer loop index");
  }
  if (index.int_value == std::numeric_limits<long long>::max()) {
    throw EvalException("integer overflow in while loop index increment");
  }
  ++index.int_value;
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
  const Value* rhs_ptr = nullptr;
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

  const bool rhs_depends_on_index =
      plan->rhs_is_variable && plan->rhs_variable == plan->index_name;
  const bool rhs_depends_on_accumulator =
      plan->rhs_is_variable && plan->rhs_variable == plan->accumulator_name;
  if (plan->increment_after_operation && !rhs_depends_on_index &&
      !rhs_depends_on_accumulator) {
    const long long remaining = limit - index_ptr->int_value;
    if (remaining > 0) {
      if (!eval_numeric_repeat_inplace(plan->op, *accumulator_ptr, *rhs_ptr, remaining)) {
        return false;
      }
      index_ptr->int_value = limit;
    }
    return true;
  }

  result = Value::nil();
  while (index_ptr->int_value < limit) {
    if (!plan->increment_after_operation) {
      increment_index_checked(*index_ptr);
    }

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

    if (plan->increment_after_operation) {
      increment_index_checked(*index_ptr);
    }
  }
  return true;
}

}  // namespace

Value execute_case_while(const WhileStmt& while_stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env) {
  if (env_bool_enabled_while("SPARK_WHILE_FAST_NUMERIC", true)) {
    Value fast_result = Value::nil();
    if (try_execute_fast_numeric_while(while_stmt, self, env, fast_result)) {
      return fast_result;
    }
  }

  const auto& body = while_stmt.body;
  if (body.empty()) {
    while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
    }
    return Value::nil();
  }

  Value result = Value::nil();
  if (body.size() == 1) {
    const auto& only = *body.front();
    while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
      result = self.execute(only, env);
    }
    return result;
  }

  while (self.truthy(self.evaluate(*while_stmt.condition, env))) {
    for (const auto& child : body) {
      result = self.execute(*child, env);
    }
  }
  return result;
}

}  // namespace spark
