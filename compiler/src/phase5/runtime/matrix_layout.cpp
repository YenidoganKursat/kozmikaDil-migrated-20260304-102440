#include <stdexcept>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

Value make_matrix_from_layout(std::size_t rows, std::size_t cols, const std::vector<Value>& data) {
  return Value::matrix_value_of(rows, cols, data);
}

const Value::MatrixValue* as_matrix_ptr(const Value& value) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    return nullptr;
  }
  return value.matrix_value.get();
}

std::size_t matrix_row_count(const Value& matrix) {
  const auto* layout = as_matrix_ptr(matrix);
  return layout ? layout->rows : 0;
}

std::size_t matrix_col_count(const Value& matrix) {
  const auto* layout = as_matrix_ptr(matrix);
  return layout ? layout->cols : 0;
}

std::size_t matrix_value_count(const Value& matrix) {
  const auto* layout = as_matrix_ptr(matrix);
  return layout ? layout->data.size() : 0;
}

long long matrix_element_index(const Value& matrix, long long row, long long col) {
  const auto rows = matrix_row_count(matrix);
  const auto cols = matrix_col_count(matrix);
  const auto data_size = matrix_value_count(matrix);
  if (row < 0 || col < 0 || static_cast<std::size_t>(row) >= rows || static_cast<std::size_t>(col) >= cols) {
    throw EvalException("matrix index out of range");
  }
  const auto flat = static_cast<std::size_t>(row) * cols + static_cast<std::size_t>(col);
  if (flat >= data_size) {
    throw EvalException("matrix index out of range");
  }
  return static_cast<long long>(flat);
}

Value matrix_row_as_list(const Value& matrix, long long row) {
  const auto* layout = as_matrix_ptr(matrix);
  if (!layout) {
    throw EvalException("invalid matrix value");
  }

  const std::size_t columns = layout->cols;
  const auto normalized = normalize_matrix_index(row, layout->rows);
  if (normalized < 0 || static_cast<std::size_t>(normalized) >= layout->rows) {
    throw EvalException("matrix index out of range");
  }

  std::vector<Value> out;
  out.reserve(columns);
  const auto base = static_cast<std::size_t>(normalized) * columns;
  for (std::size_t c = 0; c < columns; ++c) {
    out.push_back(layout->data[base + c]);
  }
  return Value::list_value_of(std::move(out));
}

Value matrix_slice_rows(const Value& matrix, const std::vector<std::size_t>& rows) {
  const auto* layout = as_matrix_ptr(matrix);
  if (!layout) {
    throw EvalException("invalid matrix value");
  }

  std::vector<Value> out;
  out.reserve(rows.size() * layout->cols);
  for (const auto row : rows) {
    if (row >= layout->rows) {
      throw EvalException("matrix index out of range");
    }
    const auto base = row * layout->cols;
    for (std::size_t c = 0; c < layout->cols; ++c) {
      out.push_back(layout->data[base + c]);
    }
  }
  return make_matrix_from_layout(rows.size(), layout->cols, out);
}

Value matrix_slice_block(const Value& matrix, const std::vector<std::size_t>& rows,
                        const std::vector<std::size_t>& cols) {
  const auto* layout = as_matrix_ptr(matrix);
  if (!layout) {
    throw EvalException("invalid matrix value");
  }

  std::vector<Value> out;
  out.reserve(rows.size() * cols.size());
  for (const auto row : rows) {
    if (row >= layout->rows) {
      throw EvalException("matrix index out of range");
    }
    for (const auto col : cols) {
      if (col >= layout->cols) {
        throw EvalException("matrix index out of range");
      }
      out.push_back(layout->data[row * layout->cols + col]);
    }
  }
  return make_matrix_from_layout(rows.size(), cols.size(), out);
}

Value matrix_copy(const Value& matrix) {
  const auto* layout = as_matrix_ptr(matrix);
  if (!layout) {
    throw EvalException("invalid matrix value");
  }
  return make_matrix_from_layout(layout->rows, layout->cols, layout->data);
}

Value transpose_matrix(const Value& matrix) {
  const auto* layout = as_matrix_ptr(matrix);
  if (!layout) {
    throw EvalException("invalid matrix value");
  }

  if (layout->data.empty() && layout->rows > 0 && layout->cols > 0) {
    return Value::matrix_value_of(layout->cols, layout->rows, {});
  }

  if (layout->rows == 0 || layout->cols == 0) {
    return Value::matrix_value_of(layout->cols, layout->rows, {});
  }

  std::vector<Value> out;
  out.reserve(layout->data.size());
  for (std::size_t c = 0; c < layout->cols; ++c) {
    for (std::size_t r = 0; r < layout->rows; ++r) {
      out.push_back(layout->data[r * layout->cols + c]);
    }
  }
  return Value::matrix_value_of(layout->cols, layout->rows, std::move(out));
}

}  // namespace spark
