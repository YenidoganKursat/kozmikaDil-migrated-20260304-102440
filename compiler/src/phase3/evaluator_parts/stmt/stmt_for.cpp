#include "../internal_helpers.h"

#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace spark {

namespace {

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

  if (plan.lhs_is_variable && plan.lhs_variable == target->name) {
    plan.mode = AggregatedAssignPlan::Mode::Recurrence;
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
  for (const auto& plan : plans) {
    auto* accumulator_ptr = env->get_ptr(plan.accumulator_name);
    if (!accumulator_ptr || !is_numeric_kind(*accumulator_ptr)) {
      return false;
    }

    if (plan.mode == AggregatedAssignPlan::Mode::Recurrence) {
      if (plan.rhs_is_variable &&
          (plan.rhs_variable == for_stmt.name || accumulators.count(plan.rhs_variable) != 0U)) {
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
    }
    accumulator_ptrs.push_back(accumulator_ptr);
    lhs_values.push_back(std::move(lhs));
    rhs_values.push_back(std::move(rhs));
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
      if (!eval_numeric_repeat_inplace(plans[i].op, *accumulator_ptrs[i], rhs_values[i], iterations)) {
        return false;
      }
    }
  }
  result = Value::nil();
  return true;
}

}  // namespace

Value execute_case_for(const ForStmt& for_stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env) {
  std::optional<RangeSpec> range_spec = std::nullopt;
  if (!for_stmt.is_async && env_bool_enabled_for("SPARK_FOR_FAST_RANGE", true)) {
    range_spec = parse_range_spec(for_stmt, self, env);
    if (range_spec.has_value() &&
        env_bool_enabled_for("SPARK_FOR_FAST_NUMERIC", true)) {
      Value fast_result = Value::nil();
      if (try_execute_fast_numeric_for_range(for_stmt, self, env, *range_spec, fast_result)) {
        return fast_result;
      }
    }
  }

  auto sequence = range_spec.has_value() ? Value::nil() : self.evaluate(*for_stmt.iterable, env);
  const auto& body = for_stmt.body;

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
    *loop_slot = value;
  };

  const auto execute_body = [&](Value& result) {
    if (body.empty()) {
      return;
    }
    if (body.size() == 1) {
      result = self.execute(*body.front(), env);
      return;
    }
    for (const auto& child : body) {
      result = self.execute(*child, env);
    }
  };

  if (!for_stmt.is_async && range_spec.has_value()) {
    Value result = Value::nil();
    if (range_spec->step > 0) {
      for (long long i = range_spec->start; i < range_spec->stop; i += range_spec->step) {
        bind_loop_var(Value::int_value_of(i));
        execute_body(result);
      }
    } else {
      for (long long i = range_spec->start; i > range_spec->stop; i += range_spec->step) {
        bind_loop_var(Value::int_value_of(i));
        execute_body(result);
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
      execute_body(result);
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
      execute_body(result);
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
      execute_body(result);
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
    execute_body(result);
  }
  return result;
}

}  // namespace spark
