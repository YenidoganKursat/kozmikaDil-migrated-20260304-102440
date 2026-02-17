#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

void assign_indexed_expression(const ExprEvaluator& evaluator, Value& target,
                              const std::vector<const IndexExpr::IndexItem*>& indices,
                              std::size_t position,
                              const std::shared_ptr<Environment>& env, const Value& value) {
  if (position >= indices.size()) {
    return;
  }

  if (indices[position]->is_slice) {
    throw EvalException("slice assignment is not supported");
  }

  if (target.kind == Value::Kind::Matrix) {
    const auto row = normalize_index_value(
        value_to_int(evaluator(*indices[position]->index, env)), matrix_row_count(target));
    if (row < 0 || static_cast<std::size_t>(row) >= matrix_row_count(target)) {
      throw EvalException("matrix index out of range");
    }
    if (position + 1 >= indices.size()) {
      if (value.kind == Value::Kind::Matrix && (!value.matrix_value)) {
        throw EvalException("matrix row assignment value has invalid matrix payload");
      }
      Value normalized_row_value = value;
      if (value.kind == Value::Kind::Matrix) {
        const auto matrix_rows = value.matrix_value->rows;
        const auto matrix_cols = value.matrix_value->cols;
        if (value.matrix_value->rows != 1) {
          throw EvalException("matrix row assignment expects a list value, got Matrix with " +
                             std::to_string(matrix_rows) + "x" + std::to_string(matrix_cols));
        }
        if (value.matrix_value->cols != matrix_col_count(target)) {
          throw EvalException("matrix row assignment has wrong column count");
        }
        std::vector<Value> row;
        row.reserve(value.matrix_value->cols);
        const auto base = static_cast<std::size_t>(0) * value.matrix_value->cols;
        for (std::size_t c = 0; c < value.matrix_value->cols; ++c) {
          row.push_back(value.matrix_value->data[base + c]);
        }
        normalized_row_value = Value::list_value_of(std::move(row));
      }

      if (normalized_row_value.kind != Value::Kind::List) {
        throw EvalException("matrix row assignment expects a list value, got kind " +
                           std::to_string(static_cast<int>(normalized_row_value.kind)));
      }
      if (normalized_row_value.list_value.size() != matrix_col_count(target)) {
        throw EvalException("matrix row assignment has wrong column count");
      }
      const auto base = static_cast<std::size_t>(row) * matrix_col_count(target);
      for (std::size_t c = 0; c < matrix_col_count(target); ++c) {
        target.matrix_value->data[base + c] = normalized_row_value.list_value[c];
      }
      invalidate_matrix_cache(target);
      return;
    }

    const auto col = normalize_index_value(
        value_to_int(evaluator(*indices[position + 1]->index, env)),
        matrix_col_count(target));
    if (col < 0 || static_cast<std::size_t>(col) >= matrix_col_count(target)) {
      throw EvalException("matrix index out of range");
    }
    if (position + 2 != indices.size()) {
      throw EvalException("matrix element assignment supports only two indices");
    }
    const auto flat = static_cast<std::size_t>(row) * matrix_col_count(target) +
                      static_cast<std::size_t>(col);
    target.matrix_value->data[flat] = value;
    invalidate_matrix_cache(target);
    return;
  }

  if (target.kind != Value::Kind::List) {
    throw EvalException("index assignment target is not a list");
  }
  const auto idx = value_to_int(evaluator(*indices[position]->index, env));
  const auto normalized = normalize_index_value(idx, target.list_value.size());
  if (normalized < 0 || static_cast<std::size_t>(normalized) >= target.list_value.size()) {
    throw EvalException("index out of range");
  }

  if (position + 1 == indices.size()) {
    target.list_value[static_cast<std::size_t>(normalized)] = value;
    invalidate_list_cache(target);
    return;
  }

  assign_indexed_expression(evaluator, target.list_value[static_cast<std::size_t>(normalized)], indices,
                           position + 1, env, value);
  invalidate_list_cache(target);
}

}  // namespace spark
