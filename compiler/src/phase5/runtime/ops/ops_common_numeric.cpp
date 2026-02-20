#include "phase5/runtime/ops/runtime_ops.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "phase3/evaluator_parts/internal_helpers.h"

namespace spark::runtime_ops {

double to_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_double(value);
  }
  throw EvalException("expected numeric value");
}

bool value_is_numeric(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double ||
         value.kind == Value::Kind::Numeric;
}

const char* binary_op_name(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add:
      return "+";
    case BinaryOp::Sub:
      return "-";
    case BinaryOp::Mul:
      return "*";
    case BinaryOp::Div:
      return "/";
    case BinaryOp::Mod:
      return "%";
    case BinaryOp::Pow:
      return "^";
    case BinaryOp::Eq:
      return "==";
    case BinaryOp::Ne:
      return "!=";
    case BinaryOp::Lt:
      return "<";
    case BinaryOp::Lte:
      return "<=";
    case BinaryOp::Gt:
      return ">";
    case BinaryOp::Gte:
      return ">=";
    case BinaryOp::And:
      return "and";
    case BinaryOp::Or:
      return "or";
  }
  return "?";
}

namespace {

std::optional<std::size_t> string_repeat_count(const Value& value) {
  if (!value_is_numeric(value)) {
    return std::nullopt;
  }
  long double raw = 0.0L;
  if (value.kind == Value::Kind::Int) {
    raw = static_cast<long double>(value.int_value);
  } else if (value.kind == Value::Kind::Double) {
    raw = static_cast<long double>(value.double_value);
  } else if (value.kind == Value::Kind::Numeric && value.numeric_value) {
    if (numeric_kind_is_int(value.numeric_value->kind)) {
      if (!value.numeric_value->parsed_int_valid && value.numeric_value->payload.empty()) {
        return std::nullopt;
      }
      const auto parsed = value_to_int(value);
      raw = static_cast<long double>(parsed);
    } else {
      raw = static_cast<long double>(numeric_value_to_double(value));
    }
  }

  if (!std::isfinite(static_cast<double>(raw)) || raw < 0.0L) {
    return std::nullopt;
  }
  const auto rounded = std::floor(raw);
  if (std::fabs(raw - rounded) > 1e-12L) {
    return std::nullopt;
  }
  constexpr long double kMaxRepeat = 1'000'000.0L;
  if (rounded > kMaxRepeat) {
    throw EvalException("string repeat count is too large");
  }
  return static_cast<std::size_t>(rounded);
}

Value repeat_string(const std::string& value, std::size_t count) {
  if (count == 0 || value.empty()) {
    return Value::string_value_of("");
  }
  std::string out;
  out.reserve(value.size() * count);
  for (std::size_t i = 0; i < count; ++i) {
    out += value;
  }
  return Value::string_value_of(std::move(out));
}

}  // namespace

Value apply_generic_container_binary(BinaryOp op, const Value& left, const Value& right) {
  if (value_is_numeric(left) && value_is_numeric(right)) {
    return eval_numeric_binary_value(op, left, right);
  }

  if (op == BinaryOp::Add) {
    return Value::string_value_of(left.to_string() + right.to_string());
  }

  if (op == BinaryOp::Mul) {
    if (left.kind == Value::Kind::String) {
      const auto count = string_repeat_count(right);
      if (count.has_value()) {
        return repeat_string(left.string_value, *count);
      }
    }
    if (right.kind == Value::Kind::String) {
      const auto count = string_repeat_count(left);
      if (count.has_value()) {
        return repeat_string(right.string_value, *count);
      }
    }
  }

  throw EvalException(std::string("non-numeric element does not support operator '") +
                      binary_op_name(op) + "'");
}

bool is_list_binary_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

bool has_list_operand(const Value& left, const Value& right) {
  return left.kind == Value::Kind::List || right.kind == Value::Kind::List;
}

bool is_matrix_binary_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::Pow;
}

bool has_matrix_operand(const Value& left, const Value& right) {
  return left.kind == Value::Kind::Matrix || right.kind == Value::Kind::Matrix;
}

bool env_bool_enabled(const char* name, bool fallback) {
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

double mod_runtime_safe(double lhs, double rhs) {
  if (rhs == 0.0) {
    throw EvalException("modulo by zero");
  }
  if (lhs >= 0.0 && rhs > 0.0) {
    const auto inv = 1.0 / rhs;
    return lhs - std::floor(lhs * inv) * rhs;
  }
  return std::fmod(lhs, rhs);
}

double pow_runtime_precise(double lhs, double rhs) {
  if (std::isfinite(rhs)) {
    const auto rounded = std::nearbyint(rhs);
    if (std::fabs(rhs - rounded) <= 1e-12 && std::fabs(rounded) <= 1'000'000.0) {
      const bool negative = rounded < 0.0;
      unsigned long long n = static_cast<unsigned long long>(negative ? -rounded : rounded);
      double result = 1.0;
      double factor = lhs;
      while (n > 0ULL) {
        if ((n & 1ULL) != 0ULL) {
          result *= factor;
        }
        n >>= 1ULL;
        if (n > 0ULL) {
          factor *= factor;
        }
      }
      if (negative) {
        if (result == 0.0) {
          return std::numeric_limits<double>::infinity();
        }
        return 1.0 / result;
      }
      return result;
    }
  }
  return std::pow(lhs, rhs);
}

double list_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_double(value);
  }
  throw EvalException("list arithmetic expects numeric operands");
}

double matrix_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == Value::Kind::Numeric) {
    return numeric_value_to_double(value);
  }
  throw EvalException("matrix arithmetic expects numeric operands");
}

bool should_return_double(const Value& left, const Value& right) {
  const auto matrix_has_double = [](const Value& value) {
    if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
      return false;
    }
    const auto total = value.matrix_value->rows * value.matrix_value->cols;
    const auto& cache = value.matrix_cache;
    if (cache.plan == Value::LayoutTag::PackedDouble &&
        cache.materialized_version == cache.version &&
        cache.promoted_f64.size() == total) {
      return true;
    }
    for (const auto& cell : value.matrix_value->data) {
      if (cell.kind == Value::Kind::Double || cell.kind == Value::Kind::Numeric) {
        return true;
      }
    }
    return false;
  };
  return left.kind == Value::Kind::Double || right.kind == Value::Kind::Double ||
         left.kind == Value::Kind::Numeric || right.kind == Value::Kind::Numeric ||
         matrix_has_double(left) || matrix_has_double(right);
}

const std::vector<double>* dense_f64_if_materialized(const Value& matrix) {
  if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
    return nullptr;
  }
  const auto total = matrix.matrix_value->rows * matrix.matrix_value->cols;
  const auto& cache = matrix.matrix_cache;
  if (cache.plan == Value::LayoutTag::PackedDouble &&
      cache.materialized_version == cache.version &&
      cache.promoted_f64.size() == total) {
    return &cache.promoted_f64;
  }
  return nullptr;
}

const std::vector<double>* dense_list_f64_if_materialized(const Value& list) {
  if (list.kind != Value::Kind::List) {
    return nullptr;
  }
  const auto size = list.list_value.size();
  const auto& cache = list.list_cache;
  if (cache.plan == Value::LayoutTag::PackedDouble &&
      cache.materialized_version == cache.version &&
      (!cache.promoted_f64.empty() || size == 0) &&
      (cache.promoted_f64.size() == size || (size == 0 && !cache.promoted_f64.empty()))) {
    return &cache.promoted_f64;
  }
  if (cache.plan == Value::LayoutTag::PromotedPackedDouble &&
      cache.materialized_version == cache.version &&
      cache.promoted_f64.size() == size) {
    return &cache.promoted_f64;
  }
  return nullptr;
}

const std::vector<double>& matrix_as_dense_numeric(const Value& matrix, std::vector<double>& scratch) {
  if (const auto* dense = dense_f64_if_materialized(matrix)) {
    return *dense;
  }
  if (!matrix.matrix_value) {
    throw EvalException("matrix arithmetic expects matrix value");
  }
  const auto total = matrix.matrix_value->rows * matrix.matrix_value->cols;
  const auto& data = matrix.matrix_value->data;
  if (data.size() != total) {
    throw EvalException("matrix arithmetic requires dense numeric payload");
  }
  scratch.resize(total);
  for (std::size_t i = 0; i < total; ++i) {
    if (!is_numeric_kind(data[i])) {
      throw EvalException("matrix arithmetic expects numeric matrix cells");
    }
    scratch[i] = matrix_number(data[i]);
  }
  return scratch;
}

const std::vector<double>& list_as_dense_numeric(const Value& list, std::vector<double>& scratch) {
  if (const auto* dense = dense_list_f64_if_materialized(list)) {
    return *dense;
  }
  if (list.kind != Value::Kind::List) {
    throw EvalException("list arithmetic expects list value");
  }
  const auto size = list.list_value.size();
  scratch.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    if (!is_numeric_kind(list.list_value[i])) {
      throw EvalException("list arithmetic expects numeric list elements");
    }
    scratch[i] = list_number(list.list_value[i]);
  }
  return scratch;
}

Value matrix_from_dense_f64(std::size_t rows, std::size_t cols, std::vector<double>&& dense,
                            std::optional<double> precomputed_sum) {
  const auto total = rows * cols;
  const bool dense_only_enabled = env_bool_enabled("SPARK_MATRIX_OPS_DENSE_ONLY", false);
  std::size_t dense_only_min = 16u * 1024u;
  if (const auto* min_env = std::getenv("SPARK_MATRIX_OPS_DENSE_ONLY_MIN")) {
    const auto parsed = std::strtoull(min_env, nullptr, 10);
    if (parsed > 0) {
      dense_only_min = static_cast<std::size_t>(parsed);
    }
  }

  std::vector<Value> out_data;
  if (!(dense_only_enabled && total >= dense_only_min)) {
    out_data.resize(total);
    for (std::size_t i = 0; i < total; ++i) {
      out_data[i] = Value::double_value_of(dense[i]);
    }
  }
  auto out = Value::matrix_value_of(rows, cols, std::move(out_data));
  out.matrix_cache.plan = Value::LayoutTag::PackedDouble;
  out.matrix_cache.live_plan = true;
  out.matrix_cache.operation = "matrix_dense_fast";
  out.matrix_cache.analyzed_version = out.matrix_cache.version;
  out.matrix_cache.materialized_version = out.matrix_cache.version;
  if (precomputed_sum.has_value()) {
    out.matrix_cache.reduced_sum_version = out.matrix_cache.version;
    out.matrix_cache.reduced_sum_value = *precomputed_sum;
    out.matrix_cache.reduced_sum_is_int = false;
  } else {
    out.matrix_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    out.matrix_cache.reduced_sum_value = 0.0;
    out.matrix_cache.reduced_sum_is_int = false;
  }
  out.matrix_cache.promoted_f64 = std::move(dense);
  return out;
}

Value list_from_dense_f64(std::vector<double>&& dense) {
  const auto size = dense.size();
  const bool dense_only_enabled = env_bool_enabled("SPARK_LIST_OPS_DENSE_ONLY", false);
  std::size_t dense_only_min = 32u * 1024u;
  if (const auto* min_env = std::getenv("SPARK_LIST_OPS_DENSE_ONLY_MIN")) {
    const auto parsed = std::strtoull(min_env, nullptr, 10);
    if (parsed > 0) {
      dense_only_min = static_cast<std::size_t>(parsed);
    }
  }

  std::vector<Value> out_data;
  if (!(dense_only_enabled && size >= dense_only_min)) {
    out_data.resize(size);
    for (std::size_t i = 0; i < size; ++i) {
      out_data[i] = Value::double_value_of(dense[i]);
    }
  }

  auto out = Value::list_value_of(std::move(out_data));
  out.list_cache.live_plan = true;
  out.list_cache.plan = Value::LayoutTag::PackedDouble;
  out.list_cache.operation = "list_dense_fast";
  out.list_cache.analyzed_version = out.list_cache.version;
  out.list_cache.materialized_version = out.list_cache.version;
  out.list_cache.promoted_f64 = std::move(dense);
  out.list_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
  out.list_cache.reduced_sum_value = 0.0;
  out.list_cache.reduced_sum_is_int = false;
  return out;
}

}  // namespace spark::runtime_ops
