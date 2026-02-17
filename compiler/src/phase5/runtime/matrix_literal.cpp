#include <optional>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

bool all_rows_have_same_type(const std::vector<Value>& row_values, bool& force_double) {
  std::size_t reference = 0;
  for (std::size_t r = 0; r < row_values.size(); ++r) {
    if (row_values[r].kind != Value::Kind::List) {
      return false;
    }
    if (r == 0) {
      reference = row_values[r].list_value.size();
    } else if (row_values[r].list_value.size() != reference) {
      return false;
    }
  }

  for (const auto& row : row_values) {
    for (const auto& element : row.list_value) {
      if (!is_numeric_kind(element)) {
        continue;
      }
      if (element.kind == Value::Kind::Double) {
        force_double = true;
      }
    }
  }
  return true;
}

std::optional<Value> evaluate_as_matrix_literal(const ExprEvaluator& evaluator, const ListExpr& list,
                                               const std::shared_ptr<Environment>& env) {
  if (list.elements.empty()) {
    return std::nullopt;
  }

  std::vector<Value> rows;
  rows.reserve(list.elements.size());
  for (const auto& element : list.elements) {
    auto value = evaluator(*element, env);
    if (value.kind != Value::Kind::List) {
      return std::nullopt;
    }
    rows.push_back(std::move(value));
  }

  const auto column_count = rows.front().list_value.size();
  for (const auto& row : rows) {
    if (row.kind != Value::Kind::List) {
      return std::nullopt;
    }
    if (row.list_value.size() != column_count) {
      throw EvalException("matrix rows have different lengths");
    }
  }

  bool force_double = false;
  all_rows_have_same_type(rows, force_double);

  std::vector<Value> data;
  data.reserve(rows.size() * column_count);
  for (auto& row : rows) {
    if (row.kind != Value::Kind::List) {
      return std::nullopt;
    }
    for (auto& element : row.list_value) {
      if (force_double && is_numeric_kind(element) && element.kind == Value::Kind::Int) {
        element = Value::double_value_of(matrix_element_to_double(element));
      }
      data.push_back(element);
    }
  }

  return make_matrix_from_layout(rows.size(), column_count, data);
}

}  // namespace spark
