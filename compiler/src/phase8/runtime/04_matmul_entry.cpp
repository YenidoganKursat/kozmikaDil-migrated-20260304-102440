#include <cmath>
#include <optional>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase8 {

namespace {

bool matrix_all_int(const Value& matrix) {
  if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
    return false;
  }
  for (const auto& cell : matrix.matrix_value->data) {
    if (cell.kind != Value::Kind::Int) {
      return false;
    }
  }
  return true;
}

bool matrix_like_integer_values(const std::vector<double>& data) {
  constexpr double kTol = 1e-12;
  for (const auto value : data) {
    const auto rounded = std::llround(value);
    if (std::fabs(value - static_cast<double>(rounded)) > kTol) {
      return false;
    }
  }
  return true;
}

double as_numeric_scalar(const Value& value, const std::string& name) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException(name + " expects numeric scalar");
}

double bias_at(const Value& bias, std::size_t row, std::size_t col,
               std::size_t rows, std::size_t cols) {
  if (bias.kind == Value::Kind::Int || bias.kind == Value::Kind::Double) {
    return as_numeric_scalar(bias, "matmul_add() bias");
  }

  if (bias.kind == Value::Kind::List) {
    if (bias.list_value.size() != cols) {
      throw EvalException("matmul_add() list bias expects cols-sized vector");
    }
    return as_numeric_scalar(bias.list_value[col], "matmul_add() bias list element");
  }

  if (bias.kind != Value::Kind::Matrix || !bias.matrix_value) {
    throw EvalException("matmul_add() bias expects scalar/list/matrix");
  }
  if (bias.matrix_value->rows == rows && bias.matrix_value->cols == cols) {
    return as_numeric_scalar(bias.matrix_value->data[row * cols + col],
                             "matmul_add() matrix bias cell");
  }
  if (bias.matrix_value->rows == 1 && bias.matrix_value->cols == cols) {
    return as_numeric_scalar(bias.matrix_value->data[col], "matmul_add() row bias");
  }
  if (bias.matrix_value->rows == rows && bias.matrix_value->cols == 1) {
    return as_numeric_scalar(bias.matrix_value->data[row], "matmul_add() col bias");
  }

  throw EvalException("matmul_add() bias shape mismatch");
}

double accum_at(const Value& accum, std::size_t row, std::size_t col,
                std::size_t rows, std::size_t cols) {
  if (accum.kind != Value::Kind::Matrix || !accum.matrix_value) {
    throw EvalException("matmul_axpby() expects matrix accumulator");
  }
  if (accum.matrix_value->rows != rows || accum.matrix_value->cols != cols) {
    throw EvalException("matmul_axpby() accumulator shape mismatch");
  }
  return as_numeric_scalar(accum.matrix_value->data[row * cols + col],
                           "matmul_axpby() accumulator cell");
}

Value matrix_from_f64(std::size_t rows, std::size_t cols, const std::vector<double>& data,
                      bool prefer_int) {
  std::vector<Value> out(data.size());
  const bool emit_int = prefer_int && matrix_like_integer_values(data);
  if (emit_int) {
    for (std::size_t i = 0; i < data.size(); ++i) {
      out[i].kind = Value::Kind::Int;
      out[i].int_value = std::llround(data[i]);
    }
  } else {
    for (std::size_t i = 0; i < data.size(); ++i) {
      out[i].kind = Value::Kind::Double;
      out[i].double_value = data[i];
    }
  }
  auto result = Value::matrix_value_of(rows, cols, std::move(out));
  result.matrix_cache.plan = emit_int ? Value::LayoutTag::PackedInt : Value::LayoutTag::PackedDouble;
  result.matrix_cache.live_plan = true;
  result.matrix_cache.operation = "matmul_result";
  result.matrix_cache.analyzed_version = result.matrix_cache.version;
  return result;
}

Value matrix_from_f32(std::size_t rows, std::size_t cols, const std::vector<float>& data) {
  std::vector<Value> out(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    out[i].kind = Value::Kind::Double;
    out[i].double_value = static_cast<double>(data[i]);
  }
  auto result = Value::matrix_value_of(rows, cols, std::move(out));
  result.matrix_cache.plan = Value::LayoutTag::PackedDouble;
  result.matrix_cache.live_plan = true;
  result.matrix_cache.operation = "matmul_result";
  result.matrix_cache.analyzed_version = result.matrix_cache.version;
  return result;
}

Value run_matmul_impl(Value& lhs, const Value& rhs, bool use_f32,
                      const std::optional<Value>& bias,
                      const std::optional<double>& alpha,
                      const std::optional<double>& beta,
                      const Value* accum) {
  if (lhs.kind != Value::Kind::Matrix || !lhs.matrix_value) {
    throw EvalException("matmul() receiver must be a matrix");
  }
  if (rhs.kind != Value::Kind::Matrix || !rhs.matrix_value) {
    throw EvalException("matmul() argument must be a matrix");
  }

  const auto m = lhs.matrix_value->rows;
  const auto k = lhs.matrix_value->cols;
  const auto rhs_rows = rhs.matrix_value->rows;
  const auto n = rhs.matrix_value->cols;
  if (k != rhs_rows) {
    throw EvalException("matmul() shape mismatch: lhs.cols must equal rhs.rows");
  }

  MatmulKernelIR ir;
  ir.m = m;
  ir.n = n;
  ir.k = k;
  ir.use_f32 = use_f32;
  ir.use_f64 = !use_f32;
  const auto schedule = resolve_schedule(ir);
  record_matmul_call(ir, schedule);

  if (use_f32) {
    std::vector<float> out;
    MatmulBackend backend_used = MatmulBackend::Own;
    run_matmul_f32_kernel(lhs, rhs, schedule, out, backend_used);
    std::vector<double> fused;
    if (bias.has_value() || alpha.has_value() || beta.has_value()) {
      record_epilogue_fused();
      fused.reserve(out.size());
      const auto alpha_value = alpha.value_or(1.0);
      const auto beta_value = beta.value_or(0.0);
      for (std::size_t r = 0; r < m; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
          auto value = static_cast<double>(out[r * n + c]);
          if (bias.has_value()) {
            value += bias_at(*bias, r, c, m, n);
          }
          if (alpha.has_value() || beta.has_value()) {
            const auto acc = accum ? accum_at(*accum, r, c, m, n) : 0.0;
            value = alpha_value * value + beta_value * acc;
          }
          fused.push_back(value);
        }
      }
      return matrix_from_f64(m, n, fused, false);
    }
    return matrix_from_f32(m, n, out);
  }

  std::vector<double> out;
  MatmulBackend backend_used = MatmulBackend::Own;
  run_matmul_f64_kernel(lhs, rhs, schedule, out, backend_used);

  if (bias.has_value() || alpha.has_value() || beta.has_value()) {
    record_epilogue_fused();
    const auto alpha_value = alpha.value_or(1.0);
    const auto beta_value = beta.value_or(0.0);
    for (std::size_t r = 0; r < m; ++r) {
      for (std::size_t c = 0; c < n; ++c) {
        auto value = out[r * n + c];
        if (bias.has_value()) {
          value += bias_at(*bias, r, c, m, n);
        }
        if (alpha.has_value() || beta.has_value()) {
          const auto acc = accum ? accum_at(*accum, r, c, m, n) : 0.0;
          value = alpha_value * value + beta_value * acc;
        }
        out[r * n + c] = value;
      }
    }
  }

  const bool prefer_int_output = !use_f32 && matrix_all_int(lhs) && matrix_all_int(rhs) &&
                                 !bias.has_value() && !alpha.has_value() && !beta.has_value();
  return matrix_from_f64(m, n, out, prefer_int_output);
}

}  // namespace

Value matrix_matmul_value(Value& lhs, const Value& rhs) {
  return run_matmul_impl(lhs, rhs, false, std::nullopt, std::nullopt, std::nullopt, nullptr);
}

Value matrix_matmul_f32_value(Value& lhs, const Value& rhs) {
  return run_matmul_impl(lhs, rhs, true, std::nullopt, std::nullopt, std::nullopt, nullptr);
}

Value matrix_matmul_f64_value(Value& lhs, const Value& rhs) {
  return run_matmul_impl(lhs, rhs, false, std::nullopt, std::nullopt, std::nullopt, nullptr);
}

Value matrix_matmul_add_value(Value& lhs, const Value& rhs, const Value& bias) {
  return run_matmul_impl(lhs, rhs, false, bias, std::nullopt, std::nullopt, nullptr);
}

Value matrix_matmul_axpby_value(Value& lhs, const Value& rhs, const Value& alpha,
                                const Value& beta, const Value& accum) {
  const auto alpha_value = as_numeric_scalar(alpha, "matmul_axpby() alpha");
  const auto beta_value = as_numeric_scalar(beta, "matmul_axpby() beta");
  return run_matmul_impl(lhs, rhs, false, std::nullopt, alpha_value, beta_value, &accum);
}

Value matrix_matmul_stats_value(const Value& matrix) {
  if (matrix.kind != Value::Kind::Matrix) {
    throw EvalException("matmul_stats() expects matrix receiver");
  }
  const auto& stats = matmul_stats_snapshot();
  std::vector<Value> out;
  out.reserve(12);
  out.push_back(Value::int_value_of(static_cast<long long>(stats.calls)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.own_calls)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.blas_calls)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.pack_a_count)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.pack_b_count)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.cache_hit_a)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.cache_hit_b)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.epilogue_fused_calls)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.last_m)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.last_n)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.last_k)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.last_backend)));
  return Value::list_value_of(std::move(out));
}

Value matrix_matmul_schedule_value(const Value& matrix) {
  if (matrix.kind != Value::Kind::Matrix) {
    throw EvalException("matmul_schedule() expects matrix receiver");
  }
  const auto& stats = matmul_stats_snapshot();
  const auto schedule = stats.last_schedule;
  std::vector<Value> out;
  out.reserve(10);
  out.push_back(Value::int_value_of(static_cast<long long>(schedule.backend)));
  out.push_back(Value::int_value_of(static_cast<long long>(schedule.tile_m)));
  out.push_back(Value::int_value_of(static_cast<long long>(schedule.tile_n)));
  out.push_back(Value::int_value_of(static_cast<long long>(schedule.tile_k)));
  out.push_back(Value::int_value_of(static_cast<long long>(schedule.unroll)));
  out.push_back(Value::int_value_of(static_cast<long long>(schedule.vector_width)));
  out.push_back(Value::int_value_of(schedule.pack_a ? 1 : 0));
  out.push_back(Value::int_value_of(schedule.pack_b ? 1 : 0));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.calls)));
  out.push_back(Value::int_value_of(static_cast<long long>(stats.last_backend)));
  return Value::list_value_of(std::move(out));
}

}  // namespace phase8

Value matrix_matmul_value(Value& lhs, const Value& rhs) {
  return phase8::matrix_matmul_value(lhs, rhs);
}

Value matrix_matmul_f32_value(Value& lhs, const Value& rhs) {
  return phase8::matrix_matmul_f32_value(lhs, rhs);
}

Value matrix_matmul_f64_value(Value& lhs, const Value& rhs) {
  return phase8::matrix_matmul_f64_value(lhs, rhs);
}

Value matrix_matmul_add_value(Value& lhs, const Value& rhs, const Value& bias) {
  return phase8::matrix_matmul_add_value(lhs, rhs, bias);
}

Value matrix_matmul_axpby_value(Value& lhs, const Value& rhs, const Value& alpha,
                               const Value& beta, const Value& accum) {
  return phase8::matrix_matmul_axpby_value(lhs, rhs, alpha, beta, accum);
}

Value matrix_matmul_stats_value(const Value& matrix) {
  return phase8::matrix_matmul_stats_value(matrix);
}

Value matrix_matmul_schedule_value(const Value& matrix) {
  return phase8::matrix_matmul_schedule_value(matrix);
}

}  // namespace spark
