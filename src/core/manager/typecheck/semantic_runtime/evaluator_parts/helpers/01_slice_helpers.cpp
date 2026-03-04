#include <algorithm>
#include <vector>

#include "../internal_helpers.h"

namespace spark {

long long normalize_matrix_index(long long idx, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  if (idx < 0) {
    idx += static_cast<long long>(size);
  }
  return idx;
}

void normalize_matrix_slice(long long size, long long& start, long long& stop, long long& step) {
  if (step == 0) {
    throw EvalException("slice step cannot be zero");
  }

  if (size == 0) {
    start = 0;
    stop = 0;
    return;
  }

  if (step > 0) {
    if (start < 0) {
      start = std::max(0LL, start + static_cast<long long>(size));
    }
    if (stop < 0) {
      stop = std::max(0LL, stop + static_cast<long long>(size));
    }
    if (start < 0) {
      start = 0;
    }
    if (start > static_cast<long long>(size)) {
      start = static_cast<long long>(size);
    }
    if (stop > static_cast<long long>(size)) {
      stop = static_cast<long long>(size);
    }
    return;
  }

  if (start < 0) {
    start = static_cast<long long>(size) + start;
  }
  if (start < 0) {
    start = 0;
  }
  if (start >= static_cast<long long>(size)) {
    start = static_cast<long long>(size) - 1;
  }

  if (stop < 0) {
    stop = static_cast<long long>(size) + stop;
  }
  if (stop < -1) {
    stop = -1;
  }
  if (stop >= static_cast<long long>(size)) {
    stop = static_cast<long long>(size) - 1;
  }
}

std::vector<std::size_t> matrix_range(long long size, long long start, long long stop, long long step) {
  std::vector<std::size_t> indices;
  if (step > 0) {
    for (long long i = start; i < stop; i += step) {
      if (i >= 0 && static_cast<std::size_t>(i) < static_cast<std::size_t>(size)) {
        indices.push_back(static_cast<std::size_t>(i));
      }
    }
  } else {
    for (long long i = start; i > stop; i += step) {
      if (i >= 0 && static_cast<std::size_t>(i) < static_cast<std::size_t>(size)) {
        indices.push_back(static_cast<std::size_t>(i));
      }
    }
  }
  return indices;
}

SliceBounds evaluate_slice_bounds(const ExprEvaluator& evaluator, const IndexExpr::IndexItem& item,
                                std::size_t target_size, const std::shared_ptr<Environment>& env) {
  SliceBounds bounds;
  bounds.step = item.slice_step ? value_to_int(evaluator(*item.slice_step, env)) : 1;
  if (bounds.step == 0) {
    throw EvalException("slice step cannot be zero");
  }

  if (item.slice_start) {
    bounds.start = value_to_int(evaluator(*item.slice_start, env));
  } else {
    bounds.start = (bounds.step > 0) ? 0 : (static_cast<long long>(target_size) - 1);
  }

  if (item.slice_stop) {
    bounds.stop = value_to_int(evaluator(*item.slice_stop, env));
  } else {
    bounds.stop = (bounds.step > 0) ? static_cast<long long>(target_size) : -1;
  }

  normalize_matrix_slice(static_cast<long long>(target_size), bounds.start, bounds.stop, bounds.step);
  return bounds;
}

std::vector<std::size_t> evaluate_indices_from_slice(const ExprEvaluator& evaluator, const IndexExpr::IndexItem& item,
                                                    std::size_t target_size, const std::shared_ptr<Environment>& env) {
  auto bounds = evaluate_slice_bounds(evaluator, item, target_size, env);
  return matrix_range(static_cast<long long>(target_size), bounds.start, bounds.stop, bounds.step);
}

long long normalize_index_value(long long idx, std::size_t size) {
  if (size == 0) {
    return -1;
  }
  if (idx < 0) {
    idx += static_cast<long long>(size);
  }
  return idx;
}

}  // namespace spark
