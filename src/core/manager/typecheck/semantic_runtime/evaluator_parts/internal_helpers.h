#pragma once

#include <functional>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"

namespace spark {

using ExprEvaluator = std::function<Value(const Expr&, const std::shared_ptr<Environment>&)>;

// Canonical env-flag parser used across phases (5-9). Keeping this in one place
// prevents behavioral drift between runtime/config toggles.
inline bool parse_env_flag_value(const char* raw, bool fallback) {
  if (!raw || *raw == '\0') {
    return fallback;
  }
  const std::string value(raw);
  if (value == "0" || value == "false" || value == "False" || value == "off" ||
      value == "OFF" || value == "no" || value == "NO") {
    return false;
  }
  return true;
}

inline bool env_flag_enabled(const char* name, bool fallback) {
  return parse_env_flag_value(std::getenv(name), fallback);
}

std::string double_to_string(double value);
bool is_numeric_kind(const Value& value);
double to_number_for_compare(const Value& value);
long long value_to_int(const Value& value);
double matrix_element_to_double(const Value& value);
bool matrix_element_wants_double(const Value& value);
std::string numeric_kind_to_string(Value::NumericKind kind);
Value::NumericKind numeric_kind_from_name(const std::string& name);
bool numeric_kind_is_int(Value::NumericKind kind);
bool numeric_kind_is_float(Value::NumericKind kind);
bool numeric_kind_is_high_precision_float(Value::NumericKind kind);
long double normalize_numeric_float_value(Value::NumericKind kind, long double value);
std::string high_precision_numeric_to_string(const Value::NumericValue& numeric);
std::string extended_int_numeric_to_string(const Value::NumericValue& numeric);
bool extended_int_numeric_to_i128_clamped(const Value::NumericValue& numeric, spark_i128& out);
long double extended_int_numeric_to_long_double(const Value::NumericValue& numeric);
double numeric_value_to_double(const Value& value);
long long numeric_value_to_i64(const Value& value);
bool numeric_value_is_zero(const Value& value);
Value cast_numeric_to_kind(Value::NumericKind kind, const Value& input);
bool cast_numeric_to_kind_inplace(Value::NumericKind kind, const Value& input, Value& target);
Value eval_numeric_binary_value(BinaryOp op, const Value& left, const Value& right);
bool eval_numeric_binary_value_inplace(BinaryOp op, const Value& left, const Value& right, Value& target);
bool eval_numeric_binary_arithmetic_inplace_fast(BinaryOp op, const Value& left, const Value& right,
                                                 Value& target);
bool eval_numeric_repeat_inplace(BinaryOp op, Value& target, const Value& rhs, long long iterations);
bool copy_numeric_value_inplace(Value& target, const Value& source);
void prewarm_numeric_runtime();
void initialize_high_precision_numeric_cache(Value& value);
Value bench_mixed_numeric_op_runtime(const std::string& kind_name, const std::string& op_name,
                                     long long loops, long long seed_x, long long seed_y);
void register_numeric_primitive_builtins(const std::shared_ptr<Environment>& globals);

long long normalize_matrix_index(long long idx, std::size_t size);
void normalize_matrix_slice(long long size, long long& start, long long& stop, long long& step);
std::vector<std::size_t> matrix_range(long long size, long long start, long long stop, long long step);

struct SliceBounds {
  long long start = 0;
  long long stop = 0;
  long long step = 1;
};

SliceBounds evaluate_slice_bounds(const ExprEvaluator& evaluator, const IndexExpr::IndexItem& item,
                                std::size_t target_size, const std::shared_ptr<Environment>& env);

std::vector<std::size_t> evaluate_indices_from_slice(const ExprEvaluator& evaluator, const IndexExpr::IndexItem& item,
                                                    std::size_t target_size,
                                                    const std::shared_ptr<Environment>& env);

const Value::MatrixValue* as_matrix_ptr(const Value& value);
std::size_t matrix_row_count(const Value& matrix);
std::size_t matrix_col_count(const Value& matrix);
std::size_t matrix_value_count(const Value& matrix);
long long matrix_element_index(const Value& matrix, long long row, long long col);
Value matrix_row_as_list(const Value& matrix, long long row);
Value matrix_slice_rows(const Value& matrix, const std::vector<std::size_t>& rows);
Value matrix_slice_block(const Value& matrix, const std::vector<std::size_t>& rows,
                        const std::vector<std::size_t>& cols);
Value matrix_copy(const Value& matrix);
Value transpose_matrix(const Value& matrix);
void invalidate_list_cache(Value& value);
void invalidate_matrix_cache(Value& value);
Value::LayoutTag choose_list_plan(const Value& value, const std::string& operation);
Value::LayoutTag choose_matrix_plan(const Value& value, const std::string& operation);
Value list_reduce_sum_with_plan(Value& value);
Value list_map_add_with_plan(Value& value, const Value& delta);
Value matrix_reduce_sum_with_plan(Value& value);
Value list_plan_id_value(const Value& value);
Value matrix_plan_id_value(const Value& value);
Value list_cache_stats_value(const Value& value);
Value matrix_cache_stats_value(const Value& value);
Value list_cache_bytes_value(const Value& value);
Value matrix_cache_bytes_value(const Value& value);
bool try_execute_pipeline_call(const CallExpr& call, Interpreter& self,
                              const std::shared_ptr<Environment>& env, Value& out);
Value pipeline_stats_value(const std::shared_ptr<Environment>& env, const std::string& name);
Value pipeline_plan_id_value(const std::shared_ptr<Environment>& env, const std::string& name);
Value matrix_matmul_value(Value& lhs, const Value& rhs);
Value matrix_matmul_f32_value(Value& lhs, const Value& rhs);
Value matrix_matmul_f64_value(Value& lhs, const Value& rhs);
Value matrix_matmul_sum_value(Value& lhs, const Value& rhs);
Value matrix_matmul_sum_f32_value(Value& lhs, const Value& rhs);
Value matrix_matmul4_sum_value(Value& a, const Value& b, const Value& c, const Value& d);
Value matrix_matmul4_sum_f32_value(Value& a, const Value& b, const Value& c, const Value& d);
Value matrix_matmul_add_value(Value& lhs, const Value& rhs, const Value& bias);
Value matrix_matmul_axpby_value(Value& lhs, const Value& rhs, const Value& alpha,
                               const Value& beta, const Value& accum);
Value matrix_matmul_stats_value(const Value& matrix);
Value matrix_matmul_schedule_value(const Value& matrix);

// GPU interface/capability orchestrator helpers.
bool gpu_backend_supports_operation(const std::string& backend, const std::string& operation);
Value gpu_backend_builtin_value(const std::vector<Value>& args);
Value gpu_supports_builtin_value(const std::vector<Value>& args);
Value gpu_orchestrate_builtin_value(const std::vector<Value>& args);

// Phase9 concurrency runtime helpers.
Value invoke_callable_sync(const Value& callee, const std::vector<Value>& args);
Value spawn_task_value(const Value& callee, const std::vector<Value>& args,
                       const std::shared_ptr<std::atomic<bool>>& cancel_token = nullptr);
Value await_task_value(const Value& task, const std::optional<long long>& timeout_ms = std::nullopt);
Value make_task_group_value(const std::optional<long long>& timeout_ms = std::nullopt);
Value task_group_spawn_value(Value& group, const Value& callee, const std::vector<Value>& args);
Value task_group_join_all_value(Value& group);
Value task_group_cancel_all_value(Value& group);
Value parallel_for_value(const Value& start, const Value& stop, const Value& fn, const std::vector<Value>& extra_args);
Value par_map_value(const Value& list, const Value& fn);
Value par_reduce_value(const Value& list, const Value& init, const Value& fn);
Value scheduler_stats_value();
Value channel_make_value(const std::optional<long long>& capacity = std::nullopt);
Value channel_send_value(Value& channel, const Value& message);
Value channel_recv_value(Value& channel, const std::optional<long long>& timeout_ms = std::nullopt);
Value channel_close_value(Value& channel);
Value channel_stats_value(const Value& channel);
Value stream_value(Value& channel);
Value stream_next_value(Value& stream, const std::optional<long long>& timeout_ms = std::nullopt);
Value stream_has_next_value(const Value& stream);

bool all_rows_have_same_type(const std::vector<Value>& row_values, bool& force_double);
std::optional<Value> evaluate_as_matrix_literal(const ExprEvaluator& evaluator, const ListExpr& list,
                                               const std::shared_ptr<Environment>& env);

struct AssignmentRoot {
  const VariableExpr* variable = nullptr;
  std::vector<const IndexExpr::IndexItem*> indices;
};

struct IndexChain {
  const Expr* root = nullptr;
  std::vector<const IndexExpr::IndexItem*> indices;
};

IndexChain flatten_index_chain(const Expr& expr);
AssignmentRoot flatten_index_target(const Expr& expr);
long long normalize_index_value(long long idx, std::size_t size);

Value evaluate_slice(const ExprEvaluator& evaluator, const Value& target, const IndexExpr::IndexItem& item,
                    const std::shared_ptr<Environment>& env);
Value evaluate_indexed_expression(const ExprEvaluator& evaluator, const Value& target,
                                 const std::vector<const IndexExpr::IndexItem*>& indices,
                                 const std::shared_ptr<Environment>& env);
void assign_indexed_expression(const ExprEvaluator& evaluator, Value& target,
                              const std::vector<const IndexExpr::IndexItem*>& indices,
                              std::size_t position,
                              const std::shared_ptr<Environment>& env, const Value& value);

// Value constructors / predicates.
Value make_matrix_from_layout(std::size_t rows, std::size_t cols, const std::vector<Value>& data);

// Expression handlers.
Value evaluate_case_number(const NumberExpr& expr, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value evaluate_case_string(const StringExpr& expr, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value evaluate_case_bool(const BoolExpr& expr, Interpreter& self,
                        const std::shared_ptr<Environment>& env);
Value evaluate_case_variable(const VariableExpr& expr, Interpreter& self,
                            const std::shared_ptr<Environment>& env);
Value evaluate_case_list(const ListExpr& list, Interpreter& self,
                        const std::shared_ptr<Environment>& env);
Value evaluate_case_unary(const UnaryExpr& unary, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value evaluate_case_binary(const BinaryExpr& binary, Interpreter& self,
                          const std::shared_ptr<Environment>& env);
Value evaluate_case_call(const CallExpr& call, Interpreter& self,
                        const std::shared_ptr<Environment>& env);
Value evaluate_case_attribute(const AttributeExpr& attribute, Interpreter& self,
                             const std::shared_ptr<Environment>& env);
Value evaluate_case_index(const IndexExpr& index_expr, Interpreter& self,
                         const std::shared_ptr<Environment>& env);

using FastExprEvalFn = Value (*)(const Expr*, Interpreter&, const std::shared_ptr<Environment>&);

FastExprEvalFn expr_eval_controller_for_kind(Expr::Kind kind);

inline Value execute_expr_fast(const Expr& expr, Interpreter& self,
                              const std::shared_ptr<Environment>& env) {
  const auto fn = expr_eval_controller_for_kind(expr.kind);
  if (!fn) {
    throw EvalException("unsupported expression");
  }
  return fn(&expr, self, env);
}

// Statement handlers.
Value execute_case_expression(const ExpressionStmt& stmt, Interpreter& self,
                             const std::shared_ptr<Environment>& env);
Value execute_case_assign(const AssignStmt& assign, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value execute_case_return(const ReturnStmt& stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value execute_case_break(const BreakStmt& stmt, Interpreter& self,
                        const std::shared_ptr<Environment>& env);
Value execute_case_continue(const ContinueStmt& stmt, Interpreter& self,
                           const std::shared_ptr<Environment>& env);
Value execute_case_if(const IfStmt& stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env);
Value execute_case_switch(const SwitchStmt& stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value execute_case_try_catch(const TryCatchStmt& stmt, Interpreter& self,
                            const std::shared_ptr<Environment>& env);
Value execute_case_while(const WhileStmt& stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value execute_case_for(const ForStmt& stmt, Interpreter& self,
                      const std::shared_ptr<Environment>& env);
Value execute_case_function_def(const FunctionDefStmt& stmt, Interpreter& self,
                               const std::shared_ptr<Environment>& env);
Value execute_case_class_def(const ClassDefStmt& stmt, Interpreter& self,
                            const std::shared_ptr<Environment>& env);
Value execute_case_with_task_group(const WithTaskGroupStmt& stmt, Interpreter& self,
                                  const std::shared_ptr<Environment>& env);

// Fast statement dispatcher used by hot loop/control-flow paths.
// This bypasses one layer of Interpreter::execute indirection while preserving
// the exact same semantics by routing to the same case handlers.
inline Value execute_stmt_fast(const Stmt& stmt, Interpreter& self,
                               const std::shared_ptr<Environment>& env) {
  switch (stmt.kind) {
    case Stmt::Kind::Expression:
      return execute_case_expression(static_cast<const ExpressionStmt&>(stmt), self, env);
    case Stmt::Kind::Assign:
      return execute_case_assign(static_cast<const AssignStmt&>(stmt), self, env);
    case Stmt::Kind::Return:
      return execute_case_return(static_cast<const ReturnStmt&>(stmt), self, env);
    case Stmt::Kind::Break:
      return execute_case_break(static_cast<const BreakStmt&>(stmt), self, env);
    case Stmt::Kind::Continue:
      return execute_case_continue(static_cast<const ContinueStmt&>(stmt), self, env);
    case Stmt::Kind::If:
      return execute_case_if(static_cast<const IfStmt&>(stmt), self, env);
    case Stmt::Kind::Switch:
      return execute_case_switch(static_cast<const SwitchStmt&>(stmt), self, env);
    case Stmt::Kind::TryCatch:
      return execute_case_try_catch(static_cast<const TryCatchStmt&>(stmt), self, env);
    case Stmt::Kind::While:
      return execute_case_while(static_cast<const WhileStmt&>(stmt), self, env);
    case Stmt::Kind::For:
      return execute_case_for(static_cast<const ForStmt&>(stmt), self, env);
    case Stmt::Kind::FunctionDef:
      return execute_case_function_def(static_cast<const FunctionDefStmt&>(stmt), self, env);
    case Stmt::Kind::ClassDef:
      return execute_case_class_def(static_cast<const ClassDefStmt&>(stmt), self, env);
    case Stmt::Kind::WithTaskGroup:
      return execute_case_with_task_group(static_cast<const WithTaskGroupStmt&>(stmt), self, env);
  }
  throw EvalException("unsupported statement");
}

using FastStmtExecFn = Value (*)(const Stmt*, Interpreter&, const std::shared_ptr<Environment>&);

struct FastStmtExecThunk {
  const Stmt* stmt = nullptr;
  FastStmtExecFn exec = nullptr;
};

// Controller/orchestrator contract:
// - leaf controller modules expose statement-kind mappings for their domain
//   (core, control-flow, definition/runtime)
// - one orchestrator resolves final handler selection for fast thunk paths.
FastStmtExecFn stmt_exec_controller_for_kind(Stmt::Kind kind);

inline FastStmtExecThunk make_fast_stmt_thunk(const Stmt& stmt) {
  FastStmtExecThunk out;
  out.stmt = &stmt;
  out.exec = stmt_exec_controller_for_kind(stmt.kind);
  if (!out.exec) {
    throw EvalException("unsupported statement");
  }
  return out;
}

inline Value execute_stmt_thunk(const FastStmtExecThunk& thunk, Interpreter& self,
                                const std::shared_ptr<Environment>& env) {
  if (!thunk.stmt || !thunk.exec) {
    throw EvalException("invalid statement thunk");
  }
  return thunk.exec(thunk.stmt, self, env);
}

}  // namespace spark
