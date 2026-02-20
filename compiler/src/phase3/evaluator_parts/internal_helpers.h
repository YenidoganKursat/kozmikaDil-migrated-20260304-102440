#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"

namespace spark {

using ExprEvaluator = std::function<Value(const Expr&, const std::shared_ptr<Environment>&)>;

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
std::string high_precision_numeric_to_string(const Value::NumericValue& numeric);
double numeric_value_to_double(const Value& value);
long long numeric_value_to_i64(const Value& value);
bool numeric_value_is_zero(const Value& value);
Value cast_numeric_to_kind(Value::NumericKind kind, const Value& input);
Value eval_numeric_binary_value(BinaryOp op, const Value& left, const Value& right);
bool eval_numeric_binary_value_inplace(BinaryOp op, const Value& left, const Value& right, Value& target);
bool eval_numeric_repeat_inplace(BinaryOp op, Value& target, const Value& rhs, long long iterations);
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

// Statement handlers.
Value execute_case_expression(const ExpressionStmt& stmt, Interpreter& self,
                             const std::shared_ptr<Environment>& env);
Value execute_case_assign(const AssignStmt& assign, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value execute_case_return(const ReturnStmt& stmt, Interpreter& self,
                         const std::shared_ptr<Environment>& env);
Value execute_case_if(const IfStmt& stmt, Interpreter& self,
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

}  // namespace spark
