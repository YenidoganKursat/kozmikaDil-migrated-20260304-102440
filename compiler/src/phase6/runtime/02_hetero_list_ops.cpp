#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

bool is_numeric_cell(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

bool env_bool_enabled_list(const char* name, bool fallback) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  const std::string text = value;
  if (text == "0" || text == "false" || text == "False" || text == "off" || text == "OFF" ||
      text == "no" || text == "NO") {
    return false;
  }
  return true;
}

Value::ElementTag cell_tag(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return Value::ElementTag::Int;
  }
  if (value.kind == Value::Kind::Double) {
    return Value::ElementTag::Double;
  }
  if (value.kind == Value::Kind::Bool) {
    return Value::ElementTag::Bool;
  }
  return Value::ElementTag::Other;
}

double numeric_to_double(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("numeric operation expects int/double values");
}

long long saturating_size_to_i64(std::size_t value) {
  constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<long long>::max());
  return static_cast<long long>(value > kMax ? kMax : value);
}

std::size_t list_cache_bytes(const Value::ListCache& cache) {
  std::size_t bytes = 0;
  bytes += cache.promoted_f64.size() * sizeof(double);
  bytes += cache.gather_values_f64.size() * sizeof(double);
  bytes += cache.gather_indices.size() * sizeof(std::size_t);
  bytes += cache.chunks.size() * sizeof(Value::ChunkRun);
  return bytes;
}

void ensure_list_plan(Value& value, const std::string& operation) {
  if (value.kind != Value::Kind::List) {
    throw EvalException(operation + "() expects a list value");
  }
  auto& cache = value.list_cache;
  if (cache.analyzed_version == cache.version && cache.operation == operation) {
    cache.cache_hit_count += 1;
    return;
  }

  const bool preserve_dense_promoted =
      value.list_value.empty() &&
      cache.materialized_version == cache.version &&
      !cache.promoted_f64.empty();
  std::vector<double> preserved_promoted;
  if (preserve_dense_promoted) {
    preserved_promoted = cache.promoted_f64;
  }

  cache.plan = choose_list_plan(value, operation);
  cache.operation = operation;
  cache.live_plan = true;
  cache.analyzed_version = cache.version;
  cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
  cache.promoted_f64.clear();
  cache.gather_values_f64.clear();
  cache.gather_indices.clear();
  cache.chunks.clear();
  cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
  cache.reduced_sum_value = 0.0;
  cache.reduced_sum_is_int = false;
  cache.analyze_count += 1;

  if (preserve_dense_promoted && cache.plan == Value::LayoutTag::PackedDouble) {
    cache.promoted_f64 = std::move(preserved_promoted);
    cache.materialized_version = cache.version;
  }
}

void materialize_list_if_needed(Value& value) {
  auto& cache = value.list_cache;
  if (cache.materialized_version == cache.version) {
    return;
  }

  if (cache.plan == Value::LayoutTag::PromotedPackedDouble) {
    cache.promoted_f64.reserve(value.list_value.size());
    for (const auto& item : value.list_value) {
      if (!is_numeric_cell(item)) {
        throw EvalException("cannot promote hetero list to f64: non numeric element present");
      }
      cache.promoted_f64.push_back(numeric_to_double(item));
    }
    cache.materialize_count += 1;
  } else if (cache.plan == Value::LayoutTag::GatherScatter) {
    cache.gather_indices.reserve(value.list_value.size());
    cache.gather_values_f64.reserve(value.list_value.size());
    for (std::size_t i = 0; i < value.list_value.size(); ++i) {
      if (!is_numeric_cell(value.list_value[i])) {
        continue;
      }
      cache.gather_indices.push_back(i);
      cache.gather_values_f64.push_back(numeric_to_double(value.list_value[i]));
    }
    cache.materialize_count += 1;
  } else if (cache.plan == Value::LayoutTag::ChunkedUnion) {
    cache.gather_indices.reserve(value.list_value.size());
    cache.gather_values_f64.reserve(value.list_value.size());
    if (!value.list_value.empty()) {
      auto current_tag = Value::ElementTag::Other;
      std::size_t run_start = 0;
      std::size_t run_length = 0;
      auto flush_run = [&cache, &run_start, &run_length, &current_tag]() {
        if (run_length == 0) {
          return;
        }
        cache.chunks.push_back(Value::ChunkRun{
            .offset = run_start,
            .length = run_length,
            .tag = current_tag,
        });
      };
      for (std::size_t i = 0; i < value.list_value.size(); ++i) {
        const auto tag = cell_tag(value.list_value[i]);
        if (run_length == 0) {
          current_tag = tag;
          run_start = i;
          run_length = 1;
          continue;
        }
        if (current_tag == tag) {
          run_length += 1;
          continue;
        }
        flush_run();
        current_tag = tag;
        run_start = i;
        run_length = 1;
      }
      flush_run();
    }
    // Reuse numeric side-cache in steady runs so chunk path avoids repeated tag conversions.
    for (std::size_t i = 0; i < value.list_value.size(); ++i) {
      if (!is_numeric_cell(value.list_value[i])) {
        continue;
      }
      cache.gather_indices.push_back(i);
      cache.gather_values_f64.push_back(numeric_to_double(value.list_value[i]));
    }
    cache.materialize_count += 1;
  }

  cache.materialized_version = cache.version;
}

}  // namespace

Value list_reduce_sum_with_plan(Value& value) {
  if (value.kind != Value::Kind::List) {
    throw EvalException("reduce_sum() expects a list value");
  }
  if (value.list_cache.reduced_sum_version == value.list_cache.version) {
    value.list_cache.cache_hit_count += 1;
    if (value.list_cache.reduced_sum_is_int) {
      return Value::int_value_of(static_cast<long long>(std::llround(value.list_cache.reduced_sum_value)));
    }
    return Value::double_value_of(value.list_cache.reduced_sum_value);
  }

  ensure_list_plan(value, "reduce_sum");
  materialize_list_if_needed(value);

  const auto plan = value.list_cache.plan;
  if (plan == Value::LayoutTag::PackedInt) {
    long long total = 0;
    for (const auto& item : value.list_value) {
      total += item.int_value;
    }
    value.list_cache.reduced_sum_version = value.list_cache.version;
    value.list_cache.reduced_sum_value = static_cast<double>(total);
    value.list_cache.reduced_sum_is_int = true;
    return Value::int_value_of(total);
  }

  double total = 0.0;
  if (plan == Value::LayoutTag::PackedDouble) {
    const auto size = value.list_value.size();
    const auto& cache = value.list_cache;
    const bool dense_ready = cache.materialized_version == cache.version &&
                             (!cache.promoted_f64.empty() || size == 0);
    if (dense_ready) {
      for (const auto entry : cache.promoted_f64) {
        total += entry;
      }
    } else {
      for (const auto& item : value.list_value) {
        total += item.double_value;
      }
    }
    value.list_cache.reduced_sum_version = value.list_cache.version;
    value.list_cache.reduced_sum_value = total;
    value.list_cache.reduced_sum_is_int = false;
    return Value::double_value_of(total);
  }
  if (plan == Value::LayoutTag::PromotedPackedDouble) {
    for (const auto entry : value.list_cache.promoted_f64) {
      total += entry;
    }
    value.list_cache.reduced_sum_version = value.list_cache.version;
    value.list_cache.reduced_sum_value = total;
    value.list_cache.reduced_sum_is_int = false;
    return Value::double_value_of(total);
  }
  if (plan == Value::LayoutTag::GatherScatter) {
    for (const auto entry : value.list_cache.gather_values_f64) {
      total += entry;
    }
    value.list_cache.reduced_sum_version = value.list_cache.version;
    value.list_cache.reduced_sum_value = total;
    value.list_cache.reduced_sum_is_int = false;
    return Value::double_value_of(total);
  }
  if (plan == Value::LayoutTag::ChunkedUnion) {
    for (const auto entry : value.list_cache.gather_values_f64) {
      total += entry;
    }
    value.list_cache.reduced_sum_version = value.list_cache.version;
    value.list_cache.reduced_sum_value = total;
    value.list_cache.reduced_sum_is_int = false;
    return Value::double_value_of(total);
  }

  for (const auto& item : value.list_value) {
    if (is_numeric_cell(item)) {
      total += numeric_to_double(item);
    }
  }
  value.list_cache.reduced_sum_version = value.list_cache.version;
  value.list_cache.reduced_sum_value = total;
  value.list_cache.reduced_sum_is_int = false;
  return Value::double_value_of(total);
}

Value list_map_add_with_plan(Value& value, const Value& delta) {
  ensure_list_plan(value, "map_add");
  materialize_list_if_needed(value);
  if (!is_numeric_cell(delta)) {
    throw EvalException("map_add() expects numeric scalar");
  }

  const auto scalar = numeric_to_double(delta);
  double reduced_sum = 0.0;
  std::size_t numeric_count = 0;
  const bool list_dense_fast = true;
  const bool dense_only_map_add = env_bool_enabled_list("SPARK_LIST_MAP_DENSE_ONLY", false);

  std::vector<Value> out = value.list_value;
  const auto write_numeric = [&out, scalar, &reduced_sum, &numeric_count](std::size_t index, double base) {
    const auto mapped = base + scalar;
    auto& cell = out[index];
    cell.kind = Value::Kind::Double;
    cell.double_value = mapped;
    reduced_sum += mapped;
    ++numeric_count;
  };

  const auto plan = value.list_cache.plan;
  if (plan == Value::LayoutTag::PackedInt || plan == Value::LayoutTag::PackedDouble) {
    std::vector<double> mapped_cache;
    if (list_dense_fast) {
      mapped_cache.resize(value.list_value.size());
    }
    if (!dense_only_map_add) {
      out.clear();
      out.resize(value.list_value.size());
    }
    for (std::size_t i = 0; i < value.list_value.size(); ++i) {
      const auto mapped = numeric_to_double(value.list_value[i]) + scalar;
      if (!dense_only_map_add) {
        auto& cell = out[i];
        cell.kind = Value::Kind::Double;
        cell.double_value = mapped;
      }
      reduced_sum += mapped;
      ++numeric_count;
      if (list_dense_fast) {
        mapped_cache[i] = mapped;
      }
    }
    auto result = dense_only_map_add ? Value::list_value_of({}) : Value::list_value_of(std::move(out));
    if (list_dense_fast) {
      result.list_cache.live_plan = true;
      result.list_cache.plan = Value::LayoutTag::PackedDouble;
      result.list_cache.operation = "map_add";
      result.list_cache.analyzed_version = result.list_cache.version;
      result.list_cache.materialized_version = result.list_cache.version;
      result.list_cache.promoted_f64 = std::move(mapped_cache);
    }
    result.list_cache.reduced_sum_version = result.list_cache.version;
    result.list_cache.reduced_sum_value = reduced_sum;
    result.list_cache.reduced_sum_is_int = false;
    return result;
  }
  if (plan == Value::LayoutTag::PromotedPackedDouble) {
    std::vector<double> mapped_cache;
    if (list_dense_fast) {
      mapped_cache.resize(value.list_cache.promoted_f64.size());
    }
    if (!dense_only_map_add) {
      out.clear();
      out.resize(value.list_cache.promoted_f64.size());
    }
    for (std::size_t i = 0; i < value.list_cache.promoted_f64.size(); ++i) {
      const auto mapped = value.list_cache.promoted_f64[i] + scalar;
      if (!dense_only_map_add) {
        auto& cell = out[i];
        cell.kind = Value::Kind::Double;
        cell.double_value = mapped;
      }
      reduced_sum += mapped;
      ++numeric_count;
      if (list_dense_fast) {
        mapped_cache[i] = mapped;
      }
    }
    auto result = dense_only_map_add ? Value::list_value_of({}) : Value::list_value_of(std::move(out));
    if (list_dense_fast) {
      result.list_cache.live_plan = true;
      result.list_cache.plan = Value::LayoutTag::PackedDouble;
      result.list_cache.operation = "map_add";
      result.list_cache.analyzed_version = result.list_cache.version;
      result.list_cache.materialized_version = result.list_cache.version;
      result.list_cache.promoted_f64 = std::move(mapped_cache);
    }
    result.list_cache.reduced_sum_version = result.list_cache.version;
    result.list_cache.reduced_sum_value = reduced_sum;
    result.list_cache.reduced_sum_is_int = false;
    return result;
  }
  if (plan == Value::LayoutTag::GatherScatter) {
    std::vector<double> gather_values;
    gather_values.reserve(value.list_cache.gather_values_f64.size());
    for (std::size_t i = 0; i < value.list_cache.gather_indices.size() &&
                            i < value.list_cache.gather_values_f64.size();
         ++i) {
      const auto mapped = value.list_cache.gather_values_f64[i] + scalar;
      write_numeric(value.list_cache.gather_indices[i], value.list_cache.gather_values_f64[i]);
      gather_values.push_back(mapped);
    }
    auto result = Value::list_value_of(std::move(out));
    result.list_cache.live_plan = true;
    result.list_cache.plan = Value::LayoutTag::GatherScatter;
    result.list_cache.operation = "map_add";
    result.list_cache.analyzed_version = result.list_cache.version;
    result.list_cache.materialized_version = result.list_cache.version;
    result.list_cache.gather_indices = value.list_cache.gather_indices;
    result.list_cache.gather_values_f64 = std::move(gather_values);
    result.list_cache.reduced_sum_version = result.list_cache.version;
    result.list_cache.reduced_sum_value = reduced_sum;
    result.list_cache.reduced_sum_is_int = false;
    return result;
  }
  if (plan == Value::LayoutTag::ChunkedUnion) {
    std::vector<double> gather_values;
    gather_values.reserve(value.list_cache.gather_values_f64.size());
    for (std::size_t i = 0; i < value.list_cache.gather_indices.size() &&
                            i < value.list_cache.gather_values_f64.size();
         ++i) {
      const auto mapped = value.list_cache.gather_values_f64[i] + scalar;
      write_numeric(value.list_cache.gather_indices[i], value.list_cache.gather_values_f64[i]);
      gather_values.push_back(mapped);
    }
    auto result = Value::list_value_of(std::move(out));
    result.list_cache.live_plan = true;
    result.list_cache.plan = Value::LayoutTag::ChunkedUnion;
    result.list_cache.operation = "map_add";
    result.list_cache.analyzed_version = result.list_cache.version;
    result.list_cache.materialized_version = result.list_cache.version;
    result.list_cache.gather_indices = value.list_cache.gather_indices;
    result.list_cache.gather_values_f64 = std::move(gather_values);
    result.list_cache.chunks = value.list_cache.chunks;
    result.list_cache.reduced_sum_version = result.list_cache.version;
    result.list_cache.reduced_sum_value = reduced_sum;
    result.list_cache.reduced_sum_is_int = false;
    return result;
  }

  // Boxed fallback: keep non-numeric elements untouched.
  for (std::size_t i = 0; i < value.list_value.size(); ++i) {
    if (is_numeric_cell(value.list_value[i])) {
      write_numeric(i, numeric_to_double(value.list_value[i]));
    }
  }
  auto result = Value::list_value_of(std::move(out));
  if (numeric_count > 0) {
    result.list_cache.reduced_sum_version = result.list_cache.version;
    result.list_cache.reduced_sum_value = reduced_sum;
    result.list_cache.reduced_sum_is_int = false;
  }
  return result;
}

Value list_plan_id_value(const Value& value) {
  const auto plan = (value.kind == Value::Kind::List && value.list_cache.analyzed_version == value.list_cache.version)
                        ? value.list_cache.plan
                        : choose_list_plan(value, "inspect");
  return Value::int_value_of(static_cast<long long>(plan));
}

Value list_cache_stats_value(const Value& value) {
  if (value.kind != Value::Kind::List) {
    throw EvalException("cache_stats() expects list receiver");
  }
  std::vector<Value> stats;
  stats.push_back(Value::int_value_of(static_cast<long long>(value.list_cache.analyze_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.list_cache.materialize_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.list_cache.cache_hit_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.list_cache.invalidation_count)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.list_cache.version)));
  stats.push_back(Value::int_value_of(static_cast<long long>(value.list_cache.plan)));
  return Value::list_value_of(std::move(stats));
}

Value list_cache_bytes_value(const Value& value) {
  if (value.kind != Value::Kind::List) {
    throw EvalException("cache_bytes() expects list receiver");
  }
  return Value::int_value_of(saturating_size_to_i64(list_cache_bytes(value.list_cache)));
}

}  // namespace spark
