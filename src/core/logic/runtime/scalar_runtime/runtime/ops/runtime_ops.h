#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "spark/evaluator.h"

namespace spark::runtime_ops {

double to_number(const Value& value);

bool is_list_binary_op(BinaryOp op);
bool has_list_operand(const Value& left, const Value& right);
bool is_matrix_binary_op(BinaryOp op);
bool has_matrix_operand(const Value& left, const Value& right);

bool env_bool_enabled(const char* name, bool fallback);
double mod_runtime_safe(double lhs, double rhs);
double pow_runtime_precise(double lhs, double rhs);
double list_number(const Value& value);
double matrix_number(const Value& value);
bool value_is_numeric(const Value& value);
const char* binary_op_name(BinaryOp op);
Value apply_generic_container_binary(BinaryOp op, const Value& left, const Value& right);

bool should_return_double(const Value& left, const Value& right);

const std::vector<double>* dense_f64_if_materialized(const Value& matrix);
const std::vector<double>* dense_list_f64_if_materialized(const Value& list);
const std::vector<double>& matrix_as_dense_numeric(const Value& matrix, std::vector<double>& scratch);
const std::vector<double>& list_as_dense_numeric(const Value& list, std::vector<double>& scratch);

// Adaptive SIMD helpers: return true when operation is handled by vector path.
bool simd_apply_binary_f64(BinaryOp op, const double* lhs, const double* rhs, double* out, std::size_t count);
bool simd_apply_binary_f64_scalar(BinaryOp op, const double* values, double scalar, double* out,
                                  std::size_t count, bool values_on_left);

Value matrix_from_dense_f64(std::size_t rows, std::size_t cols, std::vector<double>&& dense,
                            std::optional<double> precomputed_sum = std::nullopt);
Value list_from_dense_f64(std::vector<double>&& dense);

Value apply_list_list_op(const Value& left, const Value& right, BinaryOp op);
Value apply_list_scalar_op(const Value& list, const Value& scalar, BinaryOp op, bool list_on_left);

Value apply_matrix_matrix_op(const Value& left, const Value& right, BinaryOp op);
Value apply_matrix_scalar_op(const Value& matrix, const Value& scalar, BinaryOp op, bool matrix_on_left);

namespace controllers {

Value eval_unary_orchestrator(UnaryOp op, const Value& operand);
Value eval_binary_orchestrator(BinaryOp op, const Value& left, const Value& right);

}  // namespace controllers

}  // namespace spark::runtime_ops
