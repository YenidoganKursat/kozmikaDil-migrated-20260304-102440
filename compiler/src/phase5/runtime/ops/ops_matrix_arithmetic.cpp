#include "phase5/runtime/ops/runtime_ops.h"

#include <cmath>
#include <vector>

#include "phase3/evaluator_parts/internal_helpers.h"

namespace spark::runtime_ops {

namespace {

void matrix_write_cell(Value& cell, double value, bool emit_double) {
  if (emit_double) {
    cell.kind = Value::Kind::Double;
    cell.double_value = value;
    return;
  }
  cell.kind = Value::Kind::Int;
  cell.int_value = static_cast<long long>(value);
}

bool matrix_is_numeric_operable(const Value& matrix) {
  if (!matrix.matrix_value) {
    return false;
  }
  if (const auto* dense = dense_f64_if_materialized(matrix)) {
    return dense->size() == (matrix.matrix_value->rows * matrix.matrix_value->cols);
  }
  for (const auto& cell : matrix.matrix_value->data) {
    if (!value_is_numeric(cell)) {
      return false;
    }
  }
  return true;
}

}  // namespace

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
    throw EvalException("matrix shapes must match for elementwise add/sub/div/mod/pow ops");
  }

  const bool emit_double = should_return_double(left, right);
  const auto& lhs_data = left.matrix_value->data;
  const auto& rhs_data = right.matrix_value->data;
  const std::size_t total = left.matrix_value->rows * left.matrix_value->cols;
  const bool numeric_operands = matrix_is_numeric_operable(left) && matrix_is_numeric_operable(right);

  const bool dense_fast_enabled = env_bool_enabled("SPARK_MATRIX_GENERIC_FAST", true);
  if (dense_fast_enabled && emit_double && numeric_operands) {
    std::vector<double> lhs_scratch;
    std::vector<double> rhs_scratch;
    const auto& lhs_dense = matrix_as_dense_numeric(left, lhs_scratch);
    const auto& rhs_dense = matrix_as_dense_numeric(right, rhs_scratch);
    if (lhs_dense.size() == total && rhs_dense.size() == total) {
      std::vector<double> out_dense(total, 0.0);
      double out_sum = 0.0;
      if (op == BinaryOp::Add) {
        if (!simd_apply_binary_f64(BinaryOp::Add, lhs_dense.data(), rhs_dense.data(),
                                   out_dense.data(), total)) {
          for (std::size_t i = 0; i < total; ++i) {
            const auto out = lhs_dense[i] + rhs_dense[i];
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
        }
      } else if (op == BinaryOp::Sub) {
        if (!simd_apply_binary_f64(BinaryOp::Sub, lhs_dense.data(), rhs_dense.data(),
                                   out_dense.data(), total)) {
          for (std::size_t i = 0; i < total; ++i) {
            const auto out = lhs_dense[i] - rhs_dense[i];
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
        }
      } else if (op == BinaryOp::Div) {
        for (std::size_t i = 0; i < total; ++i) {
          if (rhs_dense[i] == 0.0) {
            throw EvalException("division by zero");
          }
        }
        if (!simd_apply_binary_f64(BinaryOp::Div, lhs_dense.data(), rhs_dense.data(),
                                   out_dense.data(), total)) {
          for (std::size_t i = 0; i < total; ++i) {
            const auto out = lhs_dense[i] / rhs_dense[i];
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
        }
      } else if (op == BinaryOp::Mod) {
        for (std::size_t i = 0; i < total; ++i) {
          const auto out = mod_runtime_safe(lhs_dense[i], rhs_dense[i]);
          out_dense[i] = out;
          out_sum += out;
        }
      } else if (op == BinaryOp::Pow) {
        for (std::size_t i = 0; i < total; ++i) {
          const auto out = pow_runtime_precise(lhs_dense[i], rhs_dense[i]);
          out_dense[i] = out;
          out_sum += out;
        }
      }
      return matrix_from_dense_f64(left.matrix_value->rows, left.matrix_value->cols, std::move(out_dense), out_sum);
    }
  }

  if (!numeric_operands) {
    if (op != BinaryOp::Add) {
      throw EvalException(std::string("heterogeneous matrix supports only + for non-numeric cells (got '") +
                          binary_op_name(op) + "')");
    }
    if (lhs_data.size() != total || rhs_data.size() != total) {
      throw EvalException("matrix arithmetic requires materialized matrix payload");
    }
    std::vector<Value> out_data(total);
    for (std::size_t i = 0; i < total; ++i) {
      out_data[i] = apply_generic_container_binary(op, lhs_data[i], rhs_data[i]);
    }
    return Value::matrix_value_of(left.matrix_value->rows, left.matrix_value->cols, std::move(out_data));
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
  } else if (op == BinaryOp::Pow) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(lhs_data[i]);
      const double rhs = matrix_number(rhs_data[i]);
      matrix_write_cell(out_data[i], pow_runtime_precise(lhs, rhs), true);
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
  const bool scalar_is_numeric = value_is_numeric(scalar);
  const bool numeric_operands = matrix_is_numeric_operable(matrix) && scalar_is_numeric;
  const double rhs = scalar_is_numeric ? matrix_number(scalar) : 0.0;

  const bool dense_fast_enabled = env_bool_enabled("SPARK_MATRIX_GENERIC_FAST", true);
  if (dense_fast_enabled && emit_double && numeric_operands) {
    std::vector<double> matrix_scratch;
    const auto& dense = matrix_as_dense_numeric(matrix, matrix_scratch);
    if (dense.size() == total) {
      std::vector<double> out_dense(total, 0.0);
      double out_sum = 0.0;
      if (op == BinaryOp::Add) {
        if (!simd_apply_binary_f64_scalar(BinaryOp::Add, dense.data(), rhs, out_dense.data(),
                                          total, matrix_on_left)) {
          for (std::size_t i = 0; i < total; ++i) {
            const double lhs = dense[i];
            const auto out = matrix_on_left ? lhs + rhs : rhs + lhs;
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
        }
      } else if (op == BinaryOp::Sub) {
        if (!simd_apply_binary_f64_scalar(BinaryOp::Sub, dense.data(), rhs, out_dense.data(),
                                          total, matrix_on_left)) {
          for (std::size_t i = 0; i < total; ++i) {
            const double lhs = dense[i];
            const auto out = matrix_on_left ? lhs - rhs : rhs - lhs;
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
        }
      } else if (op == BinaryOp::Mul) {
        if (!simd_apply_binary_f64_scalar(BinaryOp::Mul, dense.data(), rhs, out_dense.data(),
                                          total, matrix_on_left)) {
          for (std::size_t i = 0; i < total; ++i) {
            const auto out = dense[i] * rhs;
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
        }
      } else if (op == BinaryOp::Div) {
        if (matrix_on_left && rhs == 0.0) {
          throw EvalException("division by zero");
        }
        if (!matrix_on_left) {
          for (std::size_t i = 0; i < total; ++i) {
            const double lhs = dense[i];
            if (lhs == 0.0) {
              throw EvalException("division by zero");
            }
          }
        }
        if (!simd_apply_binary_f64_scalar(BinaryOp::Div, dense.data(), rhs, out_dense.data(),
                                          total, matrix_on_left)) {
          for (std::size_t i = 0; i < total; ++i) {
            const double lhs = dense[i];
            const auto out = matrix_on_left ? lhs / rhs : rhs / lhs;
            out_dense[i] = out;
            out_sum += out;
          }
        } else {
          for (const auto v : out_dense) {
            out_sum += v;
          }
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
      } else if (op == BinaryOp::Pow) {
        for (std::size_t i = 0; i < total; ++i) {
          const double lhs = dense[i];
          const auto out = matrix_on_left ? pow_runtime_precise(lhs, rhs)
                                          : pow_runtime_precise(rhs, lhs);
          out_dense[i] = out;
          out_sum += out;
        }
      }
      return matrix_from_dense_f64(matrix.matrix_value->rows, matrix.matrix_value->cols, std::move(out_dense), out_sum);
    }
  }

  if (!numeric_operands) {
    if (op != BinaryOp::Add && op != BinaryOp::Mul) {
      throw EvalException(std::string("heterogeneous matrix supports only + or * with non-numeric scalar (got '") +
                          binary_op_name(op) + "')");
    }
    if (matrix_data.size() != total) {
      throw EvalException("matrix arithmetic requires materialized matrix payload");
    }
    std::vector<Value> out_data(total);
    for (std::size_t i = 0; i < total; ++i) {
      const auto& lhs = matrix_data[i];
      out_data[i] = matrix_on_left ? apply_generic_container_binary(op, lhs, scalar)
                                   : apply_generic_container_binary(op, scalar, lhs);
    }
    return Value::matrix_value_of(matrix.matrix_value->rows, matrix.matrix_value->cols, std::move(out_data));
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
  } else if (op == BinaryOp::Pow) {
    for (std::size_t i = 0; i < total; ++i) {
      const double lhs = matrix_number(matrix_data[i]);
      const double value = matrix_on_left ? pow_runtime_precise(lhs, rhs)
                                          : pow_runtime_precise(rhs, lhs);
      matrix_write_cell(out_data[i], value, true);
    }
  } else {
    throw EvalException("unsupported matrix arithmetic operator");
  }
  return Value::matrix_value_of(matrix.matrix_value->rows, matrix.matrix_value->cols, std::move(out_data));
}

}  // namespace spark::runtime_ops
