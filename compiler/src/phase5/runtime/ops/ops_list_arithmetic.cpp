#include "phase5/runtime/ops/runtime_ops.h"

#include <cmath>
#include <vector>

namespace spark::runtime_ops {

namespace {

bool list_is_numeric_operable(const Value& list) {
  if (const auto* dense = dense_list_f64_if_materialized(list)) {
    return list.list_value.empty() || dense->size() == list.list_value.size();
  }
  for (const auto& item : list.list_value) {
    if (!value_is_numeric(item)) {
      return false;
    }
  }
  return true;
}

}  // namespace

Value apply_list_list_op(const Value& left, const Value& right, BinaryOp op) {
  if (left.kind != Value::Kind::List || right.kind != Value::Kind::List) {
    throw EvalException("list arithmetic expects list values");
  }
  if (left.list_value.size() != right.list_value.size()) {
    throw EvalException("list elementwise arithmetic expects equal sizes");
  }
  if (list_is_numeric_operable(left) && list_is_numeric_operable(right)) {
    std::vector<double> lhs_scratch;
    std::vector<double> rhs_scratch;
    const auto& lhs = list_as_dense_numeric(left, lhs_scratch);
    const auto& rhs = list_as_dense_numeric(right, rhs_scratch);
    std::vector<double> out(lhs.size(), 0.0);
    switch (op) {
      case BinaryOp::Add:
        if (!simd_apply_binary_f64(BinaryOp::Add, lhs.data(), rhs.data(), out.data(), lhs.size())) {
          for (std::size_t i = 0; i < lhs.size(); ++i) {
            out[i] = lhs[i] + rhs[i];
          }
        }
        break;
      case BinaryOp::Sub:
        if (!simd_apply_binary_f64(BinaryOp::Sub, lhs.data(), rhs.data(), out.data(), lhs.size())) {
          for (std::size_t i = 0; i < lhs.size(); ++i) {
            out[i] = lhs[i] - rhs[i];
          }
        }
        break;
      case BinaryOp::Mul:
        if (!simd_apply_binary_f64(BinaryOp::Mul, lhs.data(), rhs.data(), out.data(), lhs.size())) {
          for (std::size_t i = 0; i < lhs.size(); ++i) {
            out[i] = lhs[i] * rhs[i];
          }
        }
        break;
      case BinaryOp::Div:
        for (std::size_t i = 0; i < lhs.size(); ++i) {
          if (rhs[i] == 0.0) {
            throw EvalException("division by zero");
          }
        }
        if (!simd_apply_binary_f64(BinaryOp::Div, lhs.data(), rhs.data(), out.data(), lhs.size())) {
          for (std::size_t i = 0; i < lhs.size(); ++i) {
            out[i] = lhs[i] / rhs[i];
          }
        }
        break;
      case BinaryOp::Mod:
        for (std::size_t i = 0; i < lhs.size(); ++i) {
          out[i] = mod_runtime_safe(lhs[i], rhs[i]);
        }
        break;
      case BinaryOp::Pow:
        for (std::size_t i = 0; i < lhs.size(); ++i) {
          out[i] = pow_runtime_precise(lhs[i], rhs[i]);
        }
        break;
      default:
        throw EvalException("unsupported list arithmetic operator");
    }
    return list_from_dense_f64(std::move(out));
  }

  if (op != BinaryOp::Add && op != BinaryOp::Mul) {
    throw EvalException(std::string("heterogeneous list supports only + or * (got '") +
                        binary_op_name(op) + "')");
  }

  std::vector<Value> out(left.list_value.size());
  for (std::size_t i = 0; i < left.list_value.size(); ++i) {
    out[i] = apply_generic_container_binary(op, left.list_value[i], right.list_value[i]);
  }
  return Value::list_value_of(std::move(out));
}

Value apply_list_scalar_op(const Value& list, const Value& scalar, BinaryOp op, bool list_on_left) {
  if (list.kind != Value::Kind::List) {
    throw EvalException("list arithmetic expects list value");
  }
  if (list_is_numeric_operable(list) && value_is_numeric(scalar)) {
    const auto rhs = list_number(scalar);
    std::vector<double> scratch;
    const auto& values = list_as_dense_numeric(list, scratch);
    std::vector<double> out(values.size(), 0.0);
    switch (op) {
      case BinaryOp::Add:
        if (!simd_apply_binary_f64_scalar(BinaryOp::Add, values.data(), rhs, out.data(),
                                          values.size(), list_on_left)) {
          for (std::size_t i = 0; i < values.size(); ++i) {
            const auto lhs = values[i];
            out[i] = list_on_left ? lhs + rhs : rhs + lhs;
          }
        }
        break;
      case BinaryOp::Sub:
        if (!simd_apply_binary_f64_scalar(BinaryOp::Sub, values.data(), rhs, out.data(),
                                          values.size(), list_on_left)) {
          for (std::size_t i = 0; i < values.size(); ++i) {
            const auto lhs = values[i];
            out[i] = list_on_left ? lhs - rhs : rhs - lhs;
          }
        }
        break;
      case BinaryOp::Mul:
        if (!simd_apply_binary_f64_scalar(BinaryOp::Mul, values.data(), rhs, out.data(),
                                          values.size(), list_on_left)) {
          for (std::size_t i = 0; i < values.size(); ++i) {
            out[i] = values[i] * rhs;
          }
        }
        break;
      case BinaryOp::Div:
        if (list_on_left) {
          if (rhs == 0.0) {
            throw EvalException("division by zero");
          }
          if (!simd_apply_binary_f64_scalar(BinaryOp::Div, values.data(), rhs, out.data(),
                                            values.size(), true)) {
            for (std::size_t i = 0; i < values.size(); ++i) {
              out[i] = values[i] / rhs;
            }
          }
        } else {
          for (std::size_t i = 0; i < values.size(); ++i) {
            const auto lhs = values[i];
            if (lhs == 0.0) {
              throw EvalException("division by zero");
            }
          }
          if (!simd_apply_binary_f64_scalar(BinaryOp::Div, values.data(), rhs, out.data(),
                                            values.size(), false)) {
            for (std::size_t i = 0; i < values.size(); ++i) {
              out[i] = rhs / values[i];
            }
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
      case BinaryOp::Pow:
        if (list_on_left) {
          for (std::size_t i = 0; i < values.size(); ++i) {
            out[i] = pow_runtime_precise(values[i], rhs);
          }
        } else {
          for (std::size_t i = 0; i < values.size(); ++i) {
            out[i] = pow_runtime_precise(rhs, values[i]);
          }
        }
        break;
      default:
        throw EvalException("unsupported list arithmetic operator");
    }
    return list_from_dense_f64(std::move(out));
  }

  if (op != BinaryOp::Add && op != BinaryOp::Mul) {
    throw EvalException(std::string("heterogeneous list supports only + or * (got '") +
                        binary_op_name(op) + "')");
  }
  std::vector<Value> out(list.list_value.size());
  for (std::size_t i = 0; i < list.list_value.size(); ++i) {
    const auto& lhs = list.list_value[i];
    if (list_on_left) {
      out[i] = apply_generic_container_binary(op, lhs, scalar);
    } else {
      out[i] = apply_generic_container_binary(op, scalar, lhs);
    }
  }
  return Value::list_value_of(std::move(out));
}

}  // namespace spark::runtime_ops
