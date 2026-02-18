#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

double to_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("expected numeric value");
}

bool is_matrix_binary_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod;
}

bool has_matrix_operand(const Value& left, const Value& right) {
  return left.kind == Value::Kind::Matrix || right.kind == Value::Kind::Matrix;
}

bool is_list_binary_op(BinaryOp op) {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
         op == BinaryOp::Div || op == BinaryOp::Mod;
}

bool has_list_operand(const Value& left, const Value& right) {
  return left.kind == Value::Kind::List || right.kind == Value::Kind::List;
}

double list_number(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
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
  throw EvalException("matrix arithmetic expects numeric operands");
}

inline double mod_runtime_safe(double lhs, double rhs) {
  if (rhs == 0.0) {
    throw EvalException("modulo by zero");
  }
  if (lhs >= 0.0 && rhs > 0.0) {
    const auto inv = 1.0 / rhs;
    return lhs - std::floor(lhs * inv) * rhs;
  }
  return std::fmod(lhs, rhs);
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
      if (cell.kind == Value::Kind::Double) {
        return true;
      }
    }
    return false;
  };
  return left.kind == Value::Kind::Double || right.kind == Value::Kind::Double ||
         matrix_has_double(left) || matrix_has_double(right);
}

Value matrix_as_double_cell(double value) {
  return Value::double_value_of(value);
}

Value matrix_as_int_or_double_cell(double value, bool emit_double) {
  if (emit_double) {
    return matrix_as_double_cell(value);
  }
  const auto int_value = static_cast<long long>(value);
  return Value::int_value_of(int_value);
}

void matrix_write_cell(Value& cell, double value, bool emit_double) {
  if (emit_double) {
    cell.kind = Value::Kind::Double;
    cell.double_value = value;
    return;
  }
  cell.kind = Value::Kind::Int;
  cell.int_value = static_cast<long long>(value);
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

Value matrix_from_dense_f64(std::size_t rows, std::size_t cols, std::vector<double>&& dense,
                            std::optional<double> precomputed_sum = std::nullopt) {
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

Value apply_list_list_op(const Value& left, const Value& right, BinaryOp op) {
  if (left.kind != Value::Kind::List || right.kind != Value::Kind::List) {
    throw EvalException("list arithmetic expects list values");
  }
  std::vector<double> lhs_scratch;
  std::vector<double> rhs_scratch;
  const auto& lhs = list_as_dense_numeric(left, lhs_scratch);
  const auto& rhs = list_as_dense_numeric(right, rhs_scratch);
  if (lhs.size() != rhs.size()) {
    throw EvalException("list elementwise arithmetic expects equal sizes");
  }

  std::vector<double> out(lhs.size(), 0.0);
  switch (op) {
    case BinaryOp::Add:
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        out[i] = lhs[i] + rhs[i];
      }
      break;
    case BinaryOp::Sub:
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        out[i] = lhs[i] - rhs[i];
      }
      break;
    case BinaryOp::Mul:
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        out[i] = lhs[i] * rhs[i];
      }
      break;
    case BinaryOp::Div:
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (rhs[i] == 0.0) {
          throw EvalException("division by zero");
        }
        out[i] = lhs[i] / rhs[i];
      }
      break;
    case BinaryOp::Mod:
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        out[i] = mod_runtime_safe(lhs[i], rhs[i]);
      }
      break;
    default:
      throw EvalException("unsupported list arithmetic operator");
  }
  return list_from_dense_f64(std::move(out));
}

Value apply_list_scalar_op(const Value& list, const Value& scalar, BinaryOp op, bool list_on_left) {
  if (list.kind != Value::Kind::List) {
    throw EvalException("list arithmetic expects list value");
  }
  const auto rhs = list_number(scalar);
  std::vector<double> scratch;
  const auto& values = list_as_dense_numeric(list, scratch);
  std::vector<double> out(values.size(), 0.0);
  switch (op) {
    case BinaryOp::Add:
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto lhs = values[i];
        out[i] = list_on_left ? lhs + rhs : rhs + lhs;
      }
      break;
    case BinaryOp::Sub:
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto lhs = values[i];
        out[i] = list_on_left ? lhs - rhs : rhs - lhs;
      }
      break;
    case BinaryOp::Mul:
      for (std::size_t i = 0; i < values.size(); ++i) {
        out[i] = values[i] * rhs;
      }
      break;
    case BinaryOp::Div:
      if (list_on_left) {
        if (rhs == 0.0) {
          throw EvalException("division by zero");
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
          out[i] = values[i] / rhs;
        }
      } else {
        for (std::size_t i = 0; i < values.size(); ++i) {
          const auto lhs = values[i];
          if (lhs == 0.0) {
            throw EvalException("division by zero");
          }
          out[i] = rhs / lhs;
        }
      }
      break;
    case BinaryOp::Mod:
      if (list_on_left) {
        if (rhs == 0.0) {
          throw EvalException("modulo by zero");
        }
        if (rhs > 0.0) {
          const auto inv_rhs = 1.0 / rhs;
          for (std::size_t i = 0; i < values.size(); ++i) {
            const auto lhs = values[i];
            out[i] = (lhs >= 0.0) ? (lhs - std::floor(lhs * inv_rhs) * rhs) : std::fmod(lhs, rhs);
          }
        } else {
          for (std::size_t i = 0; i < values.size(); ++i) {
            out[i] = std::fmod(values[i], rhs);
          }
        }
      } else {
        for (std::size_t i = 0; i < values.size(); ++i) {
          const auto lhs = values[i];
          out[i] = mod_runtime_safe(rhs, lhs);
        }
      }
      break;
    default:
      throw EvalException("unsupported list arithmetic operator");
  }
  return list_from_dense_f64(std::move(out));
}

Value apply_matrix_matrix_op(const Value& left, const Value& right, BinaryOp op) {
  if (!left.matrix_value || !right.matrix_value) {
    throw EvalException("matrix arithmetic expects matrix values");
  }

  if (op == BinaryOp::Mul) {
    if (left.matrix_value->cols != right.matrix_value->rows) {
      throw EvalException("matrix shapes must satisfy lhs.cols == rhs.rows for matrix multiplication");
    }
    Value lhs_work = left;
    return matrix_matmul_value(lhs_work, right);
  }

  if (left.matrix_value->rows != right.matrix_value->rows || left.matrix_value->cols != right.matrix_value->cols) {
    throw EvalException("matrix shapes must match for elementwise add/sub/div/mod ops");
  }

  const bool emit_double = should_return_double(left, right);
  const auto& lhs_data = left.matrix_value->data;
  const auto& rhs_data = right.matrix_value->data;
  const std::size_t total = left.matrix_value->rows * left.matrix_value->cols;

  const bool dense_fast_enabled = env_bool_enabled("SPARK_MATRIX_GENERIC_FAST", true);
  if (dense_fast_enabled && emit_double) {
    std::vector<double> lhs_scratch;
    std::vector<double> rhs_scratch;
    const auto& lhs_dense = matrix_as_dense_numeric(left, lhs_scratch);
    const auto& rhs_dense = matrix_as_dense_numeric(right, rhs_scratch);
    if (lhs_dense.size() == total && rhs_dense.size() == total) {
      std::vector<double> out_dense(total, 0.0);
      double out_sum = 0.0;
      if (op == BinaryOp::Add) {
        for (std::size_t i = 0; i < total; ++i) {
          const auto out = lhs_dense[i] + rhs_dense[i];
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Sub) {
        for (std::size_t i = 0; i < total; ++i) {
          const auto out = lhs_dense[i] - rhs_dense[i];
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Div) {
        for (std::size_t i = 0; i < total; ++i) {
          if (rhs_dense[i] == 0.0) {
            throw EvalException("division by zero");
          }
          const auto out = lhs_dense[i] / rhs_dense[i];
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Mod) {
        for (std::size_t i = 0; i < total; ++i) {
          const auto out = mod_runtime_safe(lhs_dense[i], rhs_dense[i]);
          out_dense[i] = out;
          out_sum += out;
        }
      }
      return matrix_from_dense_f64(left.matrix_value->rows, left.matrix_value->cols,
                                   std::move(out_dense), out_sum);
    }
  }

  if (lhs_data.size() != total || rhs_data.size() != total) {
    throw EvalException("matrix arithmetic requires materialized matrix payload");
  }
  std::vector<Value> out_data(total);

  if (op == BinaryOp::Add) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(lhs_data[i]);
      const double rhs = matrix_number(rhs_data[i]);
      matrix_write_cell(out_data[i], lhs + rhs, emit_double);
    }
  } else if (op == BinaryOp::Sub) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(lhs_data[i]);
      const double rhs = matrix_number(rhs_data[i]);
      matrix_write_cell(out_data[i], lhs - rhs, emit_double);
    }
  } else if (op == BinaryOp::Div) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(lhs_data[i]);
      const double rhs = matrix_number(rhs_data[i]);
      if (rhs == 0.0) {
        throw EvalException("division by zero");
      }
      matrix_write_cell(out_data[i], lhs / rhs, true);
    }
  } else if (op == BinaryOp::Mod) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(lhs_data[i]);
      const double rhs = matrix_number(rhs_data[i]);
      matrix_write_cell(out_data[i], mod_runtime_safe(lhs, rhs), true);
    }
  } else {
    throw EvalException("unsupported matrix arithmetic operator");
  }

  return Value::matrix_value_of(left.matrix_value->rows, left.matrix_value->cols, std::move(out_data));
}

Value apply_matrix_scalar_op(const Value& matrix, const Value& scalar, BinaryOp op, bool matrix_on_left) {
  if (!matrix.matrix_value) {
    throw EvalException("matrix arithmetic expects matrix value");
  }

  const bool emit_double = should_return_double(matrix, scalar);
  const auto& matrix_data = matrix.matrix_value->data;
  const std::size_t total = matrix.matrix_value->rows * matrix.matrix_value->cols;
  const double rhs = matrix_number(scalar);

  const bool dense_fast_enabled = env_bool_enabled("SPARK_MATRIX_GENERIC_FAST", true);
  if (dense_fast_enabled && emit_double) {
    std::vector<double> matrix_scratch;
    const auto& dense = matrix_as_dense_numeric(matrix, matrix_scratch);
    if (dense.size() == total) {
      std::vector<double> out_dense(total, 0.0);
      double out_sum = 0.0;
      if (op == BinaryOp::Add) {
        for (std::size_t i = 0; i < total; ++i) {
          const double lhs = dense[i];
          const auto out = matrix_on_left ? lhs + rhs : rhs + lhs;
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Sub) {
        for (std::size_t i = 0; i < total; ++i) {
          const double lhs = dense[i];
          const auto out = matrix_on_left ? lhs - rhs : rhs - lhs;
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Mul) {
        for (std::size_t i = 0; i < total; ++i) {
          const auto out = dense[i] * rhs;
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Div) {
        if (matrix_on_left && rhs == 0.0) {
          throw EvalException("division by zero");
        }
        for (std::size_t i = 0; i < total; ++i) {
          const double lhs = dense[i];
          if (!matrix_on_left && lhs == 0.0) {
            throw EvalException("division by zero");
          }
          const auto out = matrix_on_left ? lhs / rhs : rhs / lhs;
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Mod) {
        if (matrix_on_left && rhs == 0.0) {
          throw EvalException("modulo by zero");
        }
        if (matrix_on_left && rhs > 0.0) {
          const auto inv_rhs = 1.0 / rhs;
          for (std::size_t i = 0; i < total; ++i) {
            const double lhs = dense[i];
            const auto out =
                (lhs >= 0.0) ? (lhs - std::floor(lhs * inv_rhs) * rhs) : std::fmod(lhs, rhs);
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (std::size_t i = 0; i < total; ++i) {
            const double lhs = dense[i];
            const auto out = matrix_on_left ? mod_runtime_safe(lhs, rhs) : mod_runtime_safe(rhs, lhs);
            out_dense[i] = out;
            out_sum += out;
          }
        }
      }
      return matrix_from_dense_f64(matrix.matrix_value->rows, matrix.matrix_value->cols,
                                   std::move(out_dense), out_sum);
    }
  }

  if (matrix_data.size() != total) {
    throw EvalException("matrix arithmetic requires materialized matrix payload");
  }
  std::vector<Value> out_data(total);

  if (op == BinaryOp::Add) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(matrix_data[i]);
      const double value = matrix_on_left ? lhs + rhs : rhs + lhs;
      matrix_write_cell(out_data[i], value, emit_double);
    }
  } else if (op == BinaryOp::Sub) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(matrix_data[i]);
      const double value = matrix_on_left ? lhs - rhs : rhs - lhs;
      matrix_write_cell(out_data[i], value, emit_double);
    }
  } else if (op == BinaryOp::Mul) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(matrix_data[i]);
      matrix_write_cell(out_data[i], lhs * rhs, emit_double);
    }
  } else if (op == BinaryOp::Div) {
    if (matrix_on_left && rhs == 0.0) {
      throw EvalException("division by zero");
    }
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(matrix_data[i]);
      if (!matrix_on_left && lhs == 0.0) {
        // scalar / matrix has no stable runtime behavior with zero cells here.
        // Keep explicit hard error to avoid silently returning inf.
        throw EvalException("division by zero");
      }
      const double value = matrix_on_left ? lhs / rhs : rhs / lhs;
      matrix_write_cell(out_data[i], value, true);
    }
  } else if (op == BinaryOp::Mod) {
    if (matrix_on_left && rhs == 0.0) {
      throw EvalException("modulo by zero");
    }
    if (matrix_on_left && rhs > 0.0) {
      const auto inv_rhs = 1.0 / rhs;
      for (std::size_t i = 0; i < total; ++i) {
        const double lhs = matrix_number(matrix_data[i]);
        const double value =
            (lhs >= 0.0) ? (lhs - std::floor(lhs * inv_rhs) * rhs) : std::fmod(lhs, rhs);
        matrix_write_cell(out_data[i], value, true);
      }
    } else {
      for (std::size_t i = 0; i < total; ++i) {
        const double lhs = matrix_number(matrix_data[i]);
        const double value = matrix_on_left ? mod_runtime_safe(lhs, rhs) : mod_runtime_safe(rhs, lhs);
        matrix_write_cell(out_data[i], value, true);
      }
    }
  } else {
    throw EvalException("unsupported matrix arithmetic operator");
  }
  return Value::matrix_value_of(matrix.matrix_value->rows, matrix.matrix_value->cols, std::move(out_data));
}

}  // namespace

double Interpreter::to_number(const Value& value) {
  return ::spark::to_number(value);
}

bool Interpreter::truthy(const Value& value) {
  switch (value.kind) {
    case Value::Kind::Nil:
      return false;
    case Value::Kind::Bool:
      return value.bool_value;
    case Value::Kind::Int:
      return value.int_value != 0;
    case Value::Kind::Double:
      return value.double_value != 0.0;
    case Value::Kind::List:
      if (!value.list_value.empty()) {
        return true;
      }
      if (value.list_cache.materialized_version == value.list_cache.version &&
          !value.list_cache.promoted_f64.empty()) {
        return true;
      }
      return false;
    default:
      return true;
  }
}

Value Interpreter::eval_unary(UnaryOp op, const Value& operand) const {
  switch (op) {
    case UnaryOp::Neg:
      if (!is_numeric_kind(operand)) {
        throw EvalException("unary - expects numeric value");
      }
      if (operand.kind == Value::Kind::Int) {
        return Value::int_value_of(-operand.int_value);
      }
      return Value::double_value_of(-operand.double_value);
    case UnaryOp::Not:
      return Value::bool_value_of(!truthy(operand));
    case UnaryOp::Await:
      return await_task_value(operand);
  }

  return Value::nil();
}

Value Interpreter::eval_binary(BinaryOp op, const Value& left, const Value& right) const {
  if (op == BinaryOp::Or || op == BinaryOp::And) {
    return Value::bool_value_of(op == BinaryOp::And ? (truthy(left) && truthy(right)) :
                                           (truthy(left) || truthy(right)));
  }

  if (op == BinaryOp::Eq) {
    return Value::bool_value_of(left.equals(right));
  }
  if (op == BinaryOp::Ne) {
    return Value::bool_value_of(!left.equals(right));
  }

  if (op == BinaryOp::Add && left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
    if (env_bool_enabled("SPARK_LIST_ADD_ELEMENTWISE", false)) {
      return apply_list_list_op(left, right, op);
    }
    std::vector<Value> result = left.list_value;
    result.insert(result.end(), right.list_value.begin(), right.list_value.end());
    return Value::list_value_of(std::move(result));
  }

  if (has_list_operand(left, right)) {
    if (!is_list_binary_op(op)) {
      throw EvalException("list arithmetic supports only +,-,*,/,%");
    }
    if (left.kind == Value::Kind::List && right.kind == Value::Kind::List) {
      return apply_list_list_op(left, right, op);
    }
    if (left.kind == Value::Kind::List && is_numeric_kind(right)) {
      return apply_list_scalar_op(left, right, op, true);
    }
    if (right.kind == Value::Kind::List && is_numeric_kind(left)) {
      return apply_list_scalar_op(right, left, op, false);
    }
    throw EvalException("list arithmetic expects list/list or list/scalar operands");
  }

  if (has_matrix_operand(left, right)) {
    if (!is_matrix_binary_op(op)) {
      throw EvalException("binary arithmetic expects numeric values");
    }
    if (left.kind == Value::Kind::Matrix && right.kind == Value::Kind::Matrix) {
      return apply_matrix_matrix_op(left, right, op);
    }
    if (left.kind == Value::Kind::Matrix && is_numeric_kind(right)) {
      return apply_matrix_scalar_op(left, right, op, true);
    }
    if (right.kind == Value::Kind::Matrix && is_numeric_kind(left)) {
      return apply_matrix_scalar_op(right, left, op, false);
    }
    throw EvalException("matrix arithmetic expects numeric matrix/scalar operands");
  }

  if (!is_numeric_kind(left) || !is_numeric_kind(right)) {
    throw EvalException("binary arithmetic expects numeric values");
  }

  const bool is_double = left.kind == Value::Kind::Double || right.kind == Value::Kind::Double;
  const double lhs = to_number(left);
  const double rhs = to_number(right);

  switch (op) {
    case BinaryOp::Sub:
      if (is_double) return Value::double_value_of(lhs - rhs);
      return Value::int_value_of(left.int_value - right.int_value);
    case BinaryOp::Mul:
      if (is_double) return Value::double_value_of(lhs * rhs);
      return Value::int_value_of(left.int_value * right.int_value);
    case BinaryOp::Div:
      if (rhs == 0.0) {
        throw EvalException("division by zero");
      }
      return Value::double_value_of(lhs / rhs);
    case BinaryOp::Mod:
      if (left.kind != Value::Kind::Int || right.kind != Value::Kind::Int) {
        throw EvalException("modulo expects integer values");
      }
      if (right.int_value == 0) {
        throw EvalException("modulo by zero");
      }
      return Value::int_value_of(left.int_value % right.int_value);
    case BinaryOp::Lt:
      return Value::bool_value_of(lhs < rhs);
    case BinaryOp::Lte:
      return Value::bool_value_of(lhs <= rhs);
    case BinaryOp::Gt:
      return Value::bool_value_of(lhs > rhs);
    case BinaryOp::Gte:
      return Value::bool_value_of(lhs >= rhs);
    case BinaryOp::Add:
      if (is_double) return Value::double_value_of(lhs + rhs);
      return Value::int_value_of(left.int_value + right.int_value);
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::And:
    case BinaryOp::Or:
      break;
  }

  throw EvalException("unsupported binary operator");
}

}  // namespace spark
