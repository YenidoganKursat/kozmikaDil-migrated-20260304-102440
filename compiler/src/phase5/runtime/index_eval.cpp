#include <string>
#include <utility>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

Value evaluate_slice(const ExprEvaluator& evaluator, const Value& target, const IndexExpr::IndexItem& item,
                    const std::shared_ptr<Environment>& env) {
  if (target.kind != Value::Kind::List && target.kind != Value::Kind::String) {
    throw EvalException("slice target is not a list/string");
  }
  const auto target_size = (target.kind == Value::Kind::String)
                               ? target.string_value.size()
                               : target.list_value.size();
  const auto size = static_cast<long long>(target_size);
  long long start = 0;
  long long stop = size;
  long long step = 1;

  if (item.slice_start) {
    start = value_to_int(evaluator(*item.slice_start, env));
  }
  if (item.slice_stop) {
    stop = value_to_int(evaluator(*item.slice_stop, env));
  }
  if (item.slice_step) {
    step = value_to_int(evaluator(*item.slice_step, env));
  }
  if (step == 0) {
    throw EvalException("slice step cannot be zero");
  }

  start = normalize_index_value(start, target_size);
  stop = normalize_index_value(stop, target_size);
  if (start < 0) {
    start = 0;
  }
  if (start > size) {
    start = size;
  }
  if (stop < 0) {
    stop = 0;
  }
  if (stop > size) {
    stop = size;
  }

  if (target.kind == Value::Kind::String) {
    std::string out;
    if (step > 0) {
      for (long long i = start; i < stop; i += step) {
        if (i >= 0 && static_cast<std::size_t>(i) < target.string_value.size()) {
          out.push_back(target.string_value[static_cast<std::size_t>(i)]);
        }
      }
    } else {
      for (long long i = start; i > stop; i += step) {
        if (i >= 0 && static_cast<std::size_t>(i) < target.string_value.size()) {
          out.push_back(target.string_value[static_cast<std::size_t>(i)]);
        }
      }
    }
    return Value::string_value_of(std::move(out));
  }

  Value out;
  out.kind = Value::Kind::List;
  if (step > 0) {
    for (long long i = start; i < stop; i += step) {
      if (i >= 0 && static_cast<std::size_t>(i) < target.list_value.size()) {
        out.list_value.push_back(target.list_value[static_cast<std::size_t>(i)]);
      }
    }
  } else {
    for (long long i = start; i > stop; i += step) {
      if (i >= 0 && static_cast<std::size_t>(i) < target.list_value.size()) {
        out.list_value.push_back(target.list_value[static_cast<std::size_t>(i)]);
      }
    }
  }
  return out;
}

Value evaluate_indexed_expression(const ExprEvaluator& evaluator, const Value& target,
                                 const std::vector<const IndexExpr::IndexItem*>& indices,
                                 const std::shared_ptr<Environment>& env) {
  Value current = target;
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const auto& item = *indices[i];

    if (current.kind == Value::Kind::Matrix) {
      if (item.is_slice) {
        const auto rows = evaluate_indices_from_slice(
            evaluator, item, matrix_row_count(current), env);
        if (i + 1 == indices.size()) {
          return matrix_slice_rows(current, rows);
        }

        const auto& next = *indices[i + 1];
        if (next.is_slice) {
          const auto cols = evaluate_indices_from_slice(
              evaluator, next, matrix_col_count(current), env);
          return matrix_slice_block(current, rows, cols);
        }

        const auto col = normalize_index_value(
            value_to_int(evaluator(*next.index, env)), matrix_col_count(current));
        if (col < 0 || static_cast<std::size_t>(col) >= matrix_col_count(current)) {
          throw EvalException("matrix index out of range");
        }

        std::vector<Value> output;
        output.reserve(rows.size());
        for (const auto row : rows) {
          const auto flat = static_cast<std::size_t>(row) * matrix_col_count(current) +
                            static_cast<std::size_t>(col);
          if (flat >= matrix_value_count(current)) {
            throw EvalException("matrix index out of range");
          }
          output.push_back(current.matrix_value->data[flat]);
        }
        return Value::list_value_of(std::move(output));
      }

      const auto row = normalize_index_value(
          value_to_int(evaluator(*item.index, env)), matrix_row_count(current));
      if (row < 0 || static_cast<std::size_t>(row) >= matrix_row_count(current)) {
        throw EvalException("matrix index out of range");
      }
      if (i + 1 >= indices.size()) {
        return matrix_row_as_list(current, row);
      }
      current = matrix_row_as_list(current, row);
      continue;
    }

    if (item.is_slice) {
      current = evaluate_slice(evaluator, current, item, env);
      continue;
    }
    if (current.kind == Value::Kind::String) {
      const long long idx = value_to_int(evaluator(*item.index, env));
      const auto normalized = normalize_index_value(idx, current.string_value.size());
      if (normalized < 0 || static_cast<std::size_t>(normalized) >= current.string_value.size()) {
        throw EvalException("index out of range");
      }
      std::string one_char;
      one_char.push_back(current.string_value[static_cast<std::size_t>(normalized)]);
      current = Value::string_value_of(std::move(one_char));
      continue;
    }
    if (current.kind != Value::Kind::List) {
      throw EvalException("indexing target is not a list/string");
    }
    const long long idx = value_to_int(evaluator(*item.index, env));
    const auto normalized = normalize_index_value(idx, current.list_value.size());
    if (normalized < 0 || static_cast<std::size_t>(normalized) >= current.list_value.size()) {
      throw EvalException("index out of range");
    }
    // Copy through a temporary to avoid assigning from a subobject of `current`
    // back into `current` (self-aliasing UB under sanitizers).
    Value next = current.list_value[static_cast<std::size_t>(normalized)];
    current = std::move(next);
  }
  return current;
}

}  // namespace spark
