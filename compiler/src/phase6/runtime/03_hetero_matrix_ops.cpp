#include <limits>
#include <string>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

bool matrix_is_numeric_cell(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

double matrix_numeric_to_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("matrix reduce_sum() expects numeric cell values");
}

long long matrix_saturating_size_to_i64(std::size_t value) {
  constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<long long>::max());
  return static_cast<long long>(value > kMax ? kMax : value);
}

std::size_t matrix_cache_bytes(const Value::MatrixCache& cache) {
  return cache.promoted_f64.size() * sizeof(double);
}

void ensure_matrix_plan(Value& value, const std::string& operation) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    throw EvalException(operation + "() expects a matrix value");
  }
  auto& cache = value.matrix_cache;
  if (cache.analyzed_version == cache.version && cache.plan != Value::LayoutTag::Unknown) {
    cache.cache_hit_count += 1;
    cache.operation = operation;
    return;
  }
  cache.plan = choose_matrix_plan(value, operation);
  cache.operation = operation;
  cache.live_plan = true;
  cache.analyzed_version = cache.version;
  cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
  cache.promoted_f64.clear();
  cache.analyze_count += 1;
}

void materialize_matrix_if_needed(Value& value) {
  auto& cache = value.matrix_cache;
  if (cache.materialized_version == cache.version) {
    return;
  }
  if (cache.plan == Value::LayoutTag::PromotedPackedDouble) {
    cache.promoted_f64.reserve(value.matrix_value->data.size());
    for (const auto& cell : value.matrix_value->data) {
      if (!matrix_is_numeric_cell(cell)) {
        throw EvalException("cannot promote matrix to f64: non numeric cell detected");
      }
      cache.promoted_f64.push_back(matrix_numeric_to_double(cell));
    }
    cache.materialize_count += 1;
  }
  cache.materialized_version = cache.version;
}

}  // namespace

Value matrix_reduce_sum_with_plan(Value& value) {
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    throw EvalException("reduce_sum() expects a matrix value");
  }
  if (value.matrix_cache.reduced_sum_version == value.matrix_cache.version) {
    value.matrix_cache.cache_hit_count += 1;
    if (value.matrix_cache.reduced_sum_is_int) {
      return Value::int_value_of(static_cast<long long>(std::llround(value.matrix_cache.reduced_sum_value)));
    }
    return Value::double_value_of(value.matrix_cache.reduced_sum_value);
  }

  ensure_matrix_plan(value, "reduce_sum");
  materialize_matrix_if_needed(value);

  const auto plan = value.matrix_cache.plan;
  if (plan == Value::LayoutTag::PackedInt) {
    long long total = 0;
    for (const auto& cell : value.matrix_value->data) {
      total += cell.int_value;
    }
    value.matrix_cache.reduced_sum_version = value.matrix_cache.version;
    value.matrix_cache.reduced_sum_value = static_cast<double>(total);
    value.matrix_cache.reduced_sum_is_int = true;
    return Value::int_value_of(total);
  }
  if (plan == Value::LayoutTag::PackedDouble) {
    double total = 0.0;
    const auto total_values = value.matrix_value->rows * value.matrix_value->cols;
    const auto& cache = value.matrix_cache;
    const bool dense_ready =
        cache.materialized_version == cache.version &&
        cache.promoted_f64.size() == total_values;
    if (dense_ready) {
      for (const auto entry : cache.promoted_f64) {
        total += entry;
      }
    } else {
      for (const auto& cell : value.matrix_value->data) {
        total += cell.double_value;
      }
    }
    value.matrix_cache.reduced_sum_version = value.matrix_cache.version;
    value.matrix_cache.reduced_sum_value = total;
    value.matrix_cache.reduced_sum_is_int = false;
    return Value::double_value_of(total);
  }
  if (plan == Value::LayoutTag::PromotedPackedDouble) {
    double total = 0.0;
    for (const auto entry : value.matrix_cache.promoted_f64) {
      total += entry;
    }
    value.matrix_cache.reduced_sum_version = value.matrix_cache.version;
    value.matrix_cache.reduced_sum_value = total;
    value.matrix_cache.reduced_sum_is_int = false;
    return Value::double_value_of(total);
  }

  double total = 0.0;
  for (const auto& cell : value.matrix_value->data) {
    if (!matrix_is_numeric_cell(cell)) {
      throw EvalException("matrix reduce_sum() requires numeric cells");
    }
    total += matrix_numeric_to_double(cell);
  }
  value.matrix_cache.reduced_sum_version = value.matrix_cache.version;
  value.matrix_cache.reduced_sum_value = total;
  value.matrix_cache.reduced_sum_is_int = false;
  return Value::double_value_of(total);
}

Value matrix_plan_id_value(const Value& value) {
  const auto plan = (value.kind == Value::Kind::Matrix && value.matrix_cache.analyzed_version == value.matrix_cache.version)
                        ? value.matrix_cache.plan
                        : choose_matrix_plan(value, "inspect");
  return Value::int_value_of(static_cast<long long>(plan));
}

Value matrix_cache_stats_value(const Value& value) {
  if (value.kind != Value::Kind::Matrix) {
    throw EvalException("cache_stats() expects matrix receiver");
  }
  std::vector<Value> stats;
  stats.push_back(Value::int_value_of(static_cast<long long>(value.matrix_cache.analyze_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.matrix_cache.materialize_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.matrix_cache.cache_hit_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.matrix_cache.invalidation_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.matrix_cache.version)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.matrix_cache.plan)));
  return Value::list_value_of(std::move(stats));
}

Value matrix_cache_bytes_value(const Value& value) {
  if (value.kind != Value::Kind::Matrix) {
    throw EvalException("cache_bytes() expects matrix receiver");
  }
  return Value::int_value_of(matrix_saturating_size_to_i64(matrix_cache_bytes(value.matrix_cache)));
}

}  // namespace spark
