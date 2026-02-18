#include <limits>
#include <string>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

Value::ElementTag element_tag_of(const Value& value) {
  switch (value.kind) {
    case Value::Kind::Int:
      return Value::ElementTag::Int;
    case Value::Kind::Double:
      return Value::ElementTag::Double;
    case Value::Kind::Bool:
      return Value::ElementTag::Bool;
    default:
      return Value::ElementTag::Other;
  }
}

bool list_cache_has_live_plan(const Value::ListCache& cache) { return cache.live_plan; }

bool matrix_cache_has_live_plan(const Value::MatrixCache& cache) { return cache.live_plan; }

}  // namespace

void invalidate_list_cache(Value& value) {
  if (value.kind != Value::Kind::List) {
    return;
  }
  auto& cache = value.list_cache;
  if (!list_cache_has_live_plan(cache)) {
    // No analyzed/materialized plan yet: skip invalidation work during bulk build.
    return;
  }
  cache.version += 1;
  cache.analyzed_version = std::numeric_limits<std::uint64_t>::max();
  cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
  cache.plan = Value::LayoutTag::Unknown;
  cache.live_plan = false;
  cache.operation.clear();
  cache.promoted_f64.clear();
  cache.gather_values_f64.clear();
  cache.gather_indices.clear();
  cache.chunks.clear();
  cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
  cache.reduced_sum_value = 0.0;
  cache.reduced_sum_is_int = false;
  cache.invalidation_count += 1;
}

void invalidate_matrix_cache(Value& value) {
  if (value.kind != Value::Kind::Matrix) {
    return;
  }
  auto& cache = value.matrix_cache;
  if (!matrix_cache_has_live_plan(cache)) {
    // Matrix cache is still cold; no plan to invalidate.
    return;
  }
  cache.version += 1;
  cache.analyzed_version = std::numeric_limits<std::uint64_t>::max();
  cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
  cache.plan = Value::LayoutTag::Unknown;
  cache.live_plan = false;
  cache.operation.clear();
  cache.promoted_f64.clear();
  cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
  cache.reduced_sum_value = 0.0;
  cache.reduced_sum_is_int = false;
  cache.invalidation_count += 1;
}

Value::LayoutTag choose_list_plan(const Value& value, const std::string& operation) {
  if (value.kind != Value::Kind::List) {
    return Value::LayoutTag::Unknown;
  }

  if (value.list_value.empty() &&
      value.list_cache.materialized_version == value.list_cache.version &&
      !value.list_cache.promoted_f64.empty()) {
    return Value::LayoutTag::PackedDouble;
  }

  std::size_t ints = 0;
  std::size_t doubles = 0;
  std::size_t bools = 0;
  std::size_t others = 0;
  for (const auto& item : value.list_value) {
    switch (element_tag_of(item)) {
      case Value::ElementTag::Int:
        ++ints;
        break;
      case Value::ElementTag::Double:
        ++doubles;
        break;
      case Value::ElementTag::Bool:
        ++bools;
        break;
      case Value::ElementTag::Other:
        ++others;
        break;
    }
  }

  if (bools == 0 && others == 0) {
    if (doubles == 0) {
      return Value::LayoutTag::PackedInt;
    }
    if (ints == 0) {
      return Value::LayoutTag::PackedDouble;
    }
    return Value::LayoutTag::PromotedPackedDouble;
  }

  const auto total = value.list_value.size();
  const auto numeric = ints + doubles;
  if ((operation == "reduce_sum" || operation == "map_add") && numeric > 0 &&
      numeric * 2 >= total && (bools + others) > 0) {
    return Value::LayoutTag::GatherScatter;
  }

  if (total >= 8 && (ints > 0 || doubles > 0) && (bools > 0 || others > 0)) {
    return Value::LayoutTag::ChunkedUnion;
  }

  return Value::LayoutTag::BoxedAny;
}

Value::LayoutTag choose_matrix_plan(const Value& value, const std::string& operation) {
  (void)operation;
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    return Value::LayoutTag::Unknown;
  }
  std::size_t ints = 0;
  std::size_t doubles = 0;
  std::size_t others = 0;
  for (const auto& cell : value.matrix_value->data) {
    if (cell.kind == Value::Kind::Int) {
      ++ints;
      continue;
    }
    if (cell.kind == Value::Kind::Double) {
      ++doubles;
      continue;
    }
    ++others;
  }

  if (others == 0) {
    if (doubles == 0) {
      return Value::LayoutTag::PackedInt;
    }
    if (ints == 0) {
      return Value::LayoutTag::PackedDouble;
    }
    return Value::LayoutTag::PromotedPackedDouble;
  }
  return Value::LayoutTag::BoxedAny;
}

}  // namespace spark
