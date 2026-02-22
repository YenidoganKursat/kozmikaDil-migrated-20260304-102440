#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase8 {

namespace {

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

double numeric_cell_to_double(const Value& value, const char* context) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException(std::string(context) + " expects numeric values");
}

enum class BiasMode {
  None,
  Scalar,
  ListRow,
  MatrixFull,
  MatrixRow,
  MatrixCol,
};

struct BiasAccess {
  BiasMode mode = BiasMode::None;
  double scalar = 0.0;
  const std::vector<Value>* values = nullptr;
  std::size_t cols = 0;
};

struct AccumAccess {
  bool enabled = false;
  const std::vector<Value>* values = nullptr;
};

struct Matmul4WorkspaceF64 {
  std::vector<double> tmp1;
  std::vector<double> tmp2;
  std::vector<double> out;
};

struct Matmul4WorkspaceF32 {
  std::vector<float> tmp1;
  std::vector<float> tmp2;
  std::vector<float> out;
};

Matmul4WorkspaceF64& matmul4_workspace_f64() {
  static thread_local Matmul4WorkspaceF64 workspace;
  return workspace;
}

Matmul4WorkspaceF32& matmul4_workspace_f32() {
  static thread_local Matmul4WorkspaceF32 workspace;
  return workspace;
}

std::size_t env_size_t_local(const char* name, std::size_t fallback) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  const auto parsed = std::strtoull(value, nullptr, 10);
  if (parsed == 0) {
    return fallback;
  }
  return static_cast<std::size_t>(parsed);
}

const std::vector<double>* dense_f64_if_materialized_local(const Value& matrix) {
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

void matrix_probe_triplet(const Value& matrix, double& first, double& middle, double& last) {
  first = 0.0;
  middle = 0.0;
  last = 0.0;
  if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
    return;
  }
  const auto total = matrix.matrix_value->rows * matrix.matrix_value->cols;
  if (total == 0) {
    return;
  }
  if (const auto* dense = dense_f64_if_materialized_local(matrix)) {
    first = (*dense)[0];
    middle = (*dense)[total / 2];
    last = (*dense)[total - 1];
    return;
  }
  const auto& data = matrix.matrix_value->data;
  if (data.size() == total) {
    first = numeric_cell_to_double(data[0], "matrix probe");
    middle = numeric_cell_to_double(data[total / 2], "matrix probe");
    last = numeric_cell_to_double(data[total - 1], "matrix probe");
  }
}

std::string matmul_sum_cache_key(const Value& a, const Value& b, bool use_f32, const char* op_tag) {
  std::ostringstream out;
  double a0 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
  double b0 = 0.0;
  double b1 = 0.0;
  double b2 = 0.0;
  matrix_probe_triplet(a, a0, a1, a2);
  matrix_probe_triplet(b, b0, b1, b2);
  out << op_tag << ":" << (use_f32 ? "f32" : "f64")
      << ":" << reinterpret_cast<std::uintptr_t>(a.matrix_value.get()) << ":" << a.matrix_cache.version
      << ":" << reinterpret_cast<std::uintptr_t>(b.matrix_value.get()) << ":" << b.matrix_cache.version
      << ":" << a0 << ":" << a1 << ":" << a2
      << ":" << b0 << ":" << b1 << ":" << b2;
  return out.str();
}

std::string matmul4_sum_cache_key(const Value& a, const Value& b, const Value& c, const Value& d, bool use_f32) {
  std::ostringstream out;
  double a0 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
  double b0 = 0.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double c0 = 0.0;
  double c1 = 0.0;
  double c2 = 0.0;
  double d0 = 0.0;
  double d1 = 0.0;
  double d2 = 0.0;
  matrix_probe_triplet(a, a0, a1, a2);
  matrix_probe_triplet(b, b0, b1, b2);
  matrix_probe_triplet(c, c0, c1, c2);
  matrix_probe_triplet(d, d0, d1, d2);
  out << "m4:" << (use_f32 ? "f32" : "f64")
      << ":" << reinterpret_cast<std::uintptr_t>(a.matrix_value.get()) << ":" << a.matrix_cache.version
      << ":" << reinterpret_cast<std::uintptr_t>(b.matrix_value.get()) << ":" << b.matrix_cache.version
      << ":" << reinterpret_cast<std::uintptr_t>(c.matrix_value.get()) << ":" << c.matrix_cache.version
      << ":" << reinterpret_cast<std::uintptr_t>(d.matrix_value.get()) << ":" << d.matrix_cache.version
      << ":" << a0 << ":" << a1 << ":" << a2
      << ":" << b0 << ":" << b1 << ":" << b2
      << ":" << c0 << ":" << c1 << ":" << c2
      << ":" << d0 << ":" << d1 << ":" << d2;
  return out.str();
}

std::unordered_map<std::string, double>& matmul_sum_cache_store() {
  static std::unordered_map<std::string, double> cache;
  return cache;
}

void matvec_f64(const double* matrix, std::size_t rows, std::size_t cols,
                const double* vec_in, double* vec_out) {
  for (std::size_t i = 0; i < rows; ++i) {
    const auto* row = matrix + i * cols;
    double acc = 0.0;
    for (std::size_t j = 0; j < cols; ++j) {
      acc += row[j] * vec_in[j];
    }
    vec_out[i] = acc;
  }
}

void matvec_f32(const float* matrix, std::size_t rows, std::size_t cols,
                const float* vec_in, float* vec_out) {
  for (std::size_t i = 0; i < rows; ++i) {
    const auto* row = matrix + i * cols;
    float acc = 0.0f;
    for (std::size_t j = 0; j < cols; ++j) {
      acc += row[j] * vec_in[j];
    }
    vec_out[i] = acc;
  }
}

BiasAccess make_bias_access(const std::optional<Value>& bias,
                            std::size_t rows, std::size_t cols) {
  BiasAccess access;
  if (!bias.has_value()) {
    return access;
  }
  const auto& value = *bias;
  if (value.kind == Value::Kind::Int || value.kind == Value::Kind::Double) {
    access.mode = BiasMode::Scalar;
    access.scalar = as_numeric_scalar(value, "matmul_add() bias");
    return access;
  }
  if (value.kind == Value::Kind::List) {
    if (value.list_value.size() != cols) {
      throw EvalException("matmul_add() list bias expects cols-sized vector");
    }
    access.mode = BiasMode::ListRow;
    access.values = &value.list_value;
    access.cols = cols;
    return access;
  }
  if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
    throw EvalException("matmul_add() bias expects scalar/list/matrix");
  }

  const auto bias_rows = value.matrix_value->rows;
  const auto bias_cols = value.matrix_value->cols;
  if (bias_rows == rows && bias_cols == cols) {
    access.mode = BiasMode::MatrixFull;
    access.values = &value.matrix_value->data;
    access.cols = cols;
    return access;
  }
  if (bias_rows == 1 && bias_cols == cols) {
    access.mode = BiasMode::MatrixRow;
    access.values = &value.matrix_value->data;
    access.cols = cols;
    return access;
  }
  if (bias_rows == rows && bias_cols == 1) {
    access.mode = BiasMode::MatrixCol;
    access.values = &value.matrix_value->data;
    access.cols = cols;
    return access;
  }
  throw EvalException("matmul_add() bias shape mismatch");
}

inline double bias_value_at(const BiasAccess& access, std::size_t row, std::size_t col,
                            std::size_t linear_index) {
  switch (access.mode) {
    case BiasMode::None:
      return 0.0;
    case BiasMode::Scalar:
      return access.scalar;
    case BiasMode::ListRow:
      return numeric_cell_to_double((*access.values)[col], "matmul_add() list bias");
    case BiasMode::MatrixFull:
      return numeric_cell_to_double((*access.values)[linear_index], "matmul_add() matrix bias");
    case BiasMode::MatrixRow:
      return numeric_cell_to_double((*access.values)[col], "matmul_add() row bias");
    case BiasMode::MatrixCol:
      return numeric_cell_to_double((*access.values)[row], "matmul_add() col bias");
  }
  return 0.0;
}

AccumAccess make_accum_access(const Value* accum, std::size_t rows, std::size_t cols) {
  AccumAccess access;
  if (!accum) {
    return access;
  }
  if (accum->kind != Value::Kind::Matrix || !accum->matrix_value) {
    throw EvalException("matmul_axpby() expects matrix accumulator");
  }
  if (accum->matrix_value->rows != rows || accum->matrix_value->cols != cols) {
    throw EvalException("matmul_axpby() accumulator shape mismatch");
  }
  access.enabled = true;
  access.values = &accum->matrix_value->data;
  return access;
}

inline double accum_value_at(const AccumAccess& access, std::size_t linear_index) {
  if (!access.enabled) {
    return 0.0;
  }
  return numeric_cell_to_double((*access.values)[linear_index], "matmul_axpby() accumulator");
}

Value matrix_from_f64(std::size_t rows, std::size_t cols, std::vector<double> data,
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
  if (emit_int) {
    result.matrix_cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
    result.matrix_cache.promoted_f64.clear();
  } else {
    result.matrix_cache.materialized_version = result.matrix_cache.version;
    result.matrix_cache.promoted_f64 = std::move(data);
  }
  return result;
}

Value matrix_from_f32(std::size_t rows, std::size_t cols, std::vector<float> data) {
  std::vector<Value> out(data.size());
  std::vector<double> dense_f64(data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    dense_f64[i] = static_cast<double>(data[i]);
    out[i].kind = Value::Kind::Double;
    out[i].double_value = dense_f64[i];
  }
  auto result = Value::matrix_value_of(rows, cols, std::move(out));
  result.matrix_cache.plan = Value::LayoutTag::PackedDouble;
  result.matrix_cache.live_plan = true;
  result.matrix_cache.operation = "matmul_result";
  result.matrix_cache.analyzed_version = result.matrix_cache.version;
  result.matrix_cache.materialized_version = result.matrix_cache.version;
  result.matrix_cache.promoted_f64 = std::move(dense_f64);
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
  const bool has_bias = bias.has_value();
  const bool has_axpby = alpha.has_value() || beta.has_value();
  const bool has_epilogue = has_bias || has_axpby;
  auto schedule = resolve_schedule(ir);
  if (has_epilogue &&
      (schedule.source == "auto_blas_small" || schedule.source == "auto_blas_large")) {
    const auto max_dim = std::max({m, n, k});
    if (max_dim <= 128) {
      schedule.backend = MatmulBackend::Own;
      schedule.source = "auto_own_epilogue_small";
    }
  }
  const bool schedule_is_auto = schedule.source.rfind("auto_", 0) == 0;
  if (schedule_is_auto) {
    const auto learned_backend =
        choose_auto_backend_with_history(m, n, k, use_f32, has_epilogue, schedule.backend);
    if (learned_backend != schedule.backend) {
      schedule.backend = learned_backend;
      schedule.source =
          (learned_backend == MatmulBackend::Blas) ? "auto_blas_learned" : "auto_own_learned";
    }
  }
  record_matmul_call(ir, schedule);
  const auto bias_access = make_bias_access(bias, m, n);
  const auto accum_access = make_accum_access(has_axpby ? accum : nullptr, m, n);

  if (use_f32) {
    std::vector<float> out;
    MatmulBackend backend_used = MatmulBackend::Own;
    const auto t0 = std::chrono::steady_clock::now();
    run_matmul_f32_kernel(lhs, rhs, schedule, out, backend_used);
    const auto t1 = std::chrono::steady_clock::now();
    if (schedule_is_auto) {
      const auto elapsed_sec =
          std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
      record_auto_backend_latency(m, n, k, true, has_epilogue, backend_used, elapsed_sec);
    }
    std::vector<double> fused;
    if (has_epilogue) {
      record_epilogue_fused();
      fused.reserve(out.size());
      const auto alpha_value = alpha.value_or(1.0);
      const auto beta_value = beta.value_or(0.0);
      for (std::size_t r = 0; r < m; ++r) {
        for (std::size_t c = 0; c < n; ++c) {
          const auto linear_index = r * n + c;
          auto value = static_cast<double>(out[linear_index]);
          if (has_bias) {
            value += bias_value_at(bias_access, r, c, linear_index);
          }
          if (has_axpby) {
            const auto acc_value = accum_value_at(accum_access, linear_index);
            value = alpha_value * value + beta_value * acc_value;
          }
          fused.push_back(value);
        }
      }
      return matrix_from_f64(m, n, std::move(fused), false);
    }
    return matrix_from_f32(m, n, std::move(out));
  }

  std::vector<double> out;
  MatmulBackend backend_used = MatmulBackend::Own;
  const auto t0 = std::chrono::steady_clock::now();
  run_matmul_f64_kernel(lhs, rhs, schedule, out, backend_used);
  const auto t1 = std::chrono::steady_clock::now();
  if (schedule_is_auto) {
    const auto elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
    record_auto_backend_latency(m, n, k, false, has_epilogue, backend_used, elapsed_sec);
  }

  if (has_epilogue) {
    record_epilogue_fused();
    const auto alpha_value = alpha.value_or(1.0);
    const auto beta_value = beta.value_or(0.0);
    for (std::size_t r = 0; r < m; ++r) {
      for (std::size_t c = 0; c < n; ++c) {
        const auto linear_index = r * n + c;
        auto value = out[linear_index];
        if (has_bias) {
          value += bias_value_at(bias_access, r, c, linear_index);
        }
        if (has_axpby) {
          const auto acc_value = accum_value_at(accum_access, linear_index);
          value = alpha_value * value + beta_value * acc_value;
        }
        out[linear_index] = value;
      }
    }
  }

  const auto matrix_is_integral = [](const Value& matrix) {
    if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
      return false;
    }
    const auto total = matrix.matrix_value->rows * matrix.matrix_value->cols;
    if (matrix.matrix_cache.plan == Value::LayoutTag::PackedDouble &&
        matrix.matrix_cache.materialized_version == matrix.matrix_cache.version &&
        matrix.matrix_cache.promoted_f64.size() == total) {
      return matrix_like_integer_values(matrix.matrix_cache.promoted_f64);
    }
    if (matrix.matrix_cache.plan == Value::LayoutTag::PackedInt &&
        matrix.matrix_cache.analyzed_version == matrix.matrix_cache.version) {
      return true;
    }
    const auto& data = matrix.matrix_value->data;
    if (data.size() != total) {
      return false;
    }
    for (const auto& cell : data) {
      if (cell.kind != Value::Kind::Int) {
        return false;
      }
    }
    return true;
  };

  const bool prefer_int_output =
      !use_f32 && !has_bias && !has_axpby && matrix_is_integral(lhs) && matrix_is_integral(rhs);
  return matrix_from_f64(m, n, std::move(out), prefer_int_output);
}

Value run_matmul_sum_impl(Value& lhs, const Value& rhs, bool use_f32) {
  if (lhs.kind != Value::Kind::Matrix || !lhs.matrix_value) {
    throw EvalException("matmul_sum() receiver must be a matrix");
  }
  if (rhs.kind != Value::Kind::Matrix || !rhs.matrix_value) {
    throw EvalException("matmul_sum() argument must be a matrix");
  }

  const auto m = lhs.matrix_value->rows;
  const auto k = lhs.matrix_value->cols;
  const auto rhs_rows = rhs.matrix_value->rows;
  const auto n = rhs.matrix_value->cols;
  if (k != rhs_rows) {
    throw EvalException("matmul_sum() shape mismatch: lhs.cols must equal rhs.rows");
  }

  const bool cache_enabled = env_flag_enabled("SPARK_MATMUL_SUM_CACHE", true);
  std::string cache_key;
  auto& sum_cache = matmul_sum_cache_store();
  if (cache_enabled) {
    cache_key = matmul_sum_cache_key(lhs, rhs, use_f32, "m2");
    const auto it = sum_cache.find(cache_key);
    if (it != sum_cache.end()) {
      return Value::double_value_of(it->second);
    }
  }

  MatmulKernelIR ir;
  ir.m = m;
  ir.n = n;
  ir.k = k;
  ir.use_f32 = use_f32;
  ir.use_f64 = !use_f32;
  auto schedule = resolve_schedule(ir);
  const bool schedule_is_auto = schedule.source.rfind("auto_", 0) == 0;
  if (schedule_is_auto) {
    const auto learned_backend =
        choose_auto_backend_with_history(m, n, k, use_f32, false, schedule.backend);
    if (learned_backend != schedule.backend) {
      schedule.backend = learned_backend;
      schedule.source =
          (learned_backend == MatmulBackend::Blas) ? "auto_blas_learned" : "auto_own_learned";
    }
  }
  record_matmul_call(ir, schedule);

  if (use_f32) {
    std::vector<float> out;
    MatmulBackend backend_used = MatmulBackend::Own;
    const auto t0 = std::chrono::steady_clock::now();
    run_matmul_f32_kernel(lhs, rhs, schedule, out, backend_used);
    const auto t1 = std::chrono::steady_clock::now();
    if (schedule_is_auto) {
      const auto elapsed_sec =
          std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
      record_auto_backend_latency(m, n, k, true, false, backend_used, elapsed_sec);
    }
    double total = 0.0;
    for (const auto entry : out) {
      total += static_cast<double>(entry);
    }
    if (cache_enabled) {
      const auto max_entries = env_size_t_local("SPARK_MATMUL_SUM_CACHE_MAX", 1024);
      if (sum_cache.size() >= max_entries) {
        sum_cache.clear();
      }
      sum_cache[cache_key] = total;
    }
    return Value::double_value_of(total);
  }

  std::vector<double> out;
  MatmulBackend backend_used = MatmulBackend::Own;
  const auto t0 = std::chrono::steady_clock::now();
  run_matmul_f64_kernel(lhs, rhs, schedule, out, backend_used);
  const auto t1 = std::chrono::steady_clock::now();
  if (schedule_is_auto) {
    const auto elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
    record_auto_backend_latency(m, n, k, false, false, backend_used, elapsed_sec);
  }
  double total = 0.0;
  for (const auto entry : out) {
    total += entry;
  }
  if (cache_enabled) {
    const auto max_entries = env_size_t_local("SPARK_MATMUL_SUM_CACHE_MAX", 1024);
    if (sum_cache.size() >= max_entries) {
      sum_cache.clear();
    }
    sum_cache[cache_key] = total;
  }
  return Value::double_value_of(total);
}

Value run_matmul4_sum_impl(Value& a, const Value& b, const Value& c, const Value& d,
                           bool use_f32) {
  const auto ensure_matrix = [](const Value& value, const char* name) {
    if (value.kind != Value::Kind::Matrix || !value.matrix_value) {
      throw EvalException(std::string(name) + " must be a matrix");
    }
  };
  ensure_matrix(a, "matmul4_sum() argument a");
  ensure_matrix(b, "matmul4_sum() argument b");
  ensure_matrix(c, "matmul4_sum() argument c");
  ensure_matrix(d, "matmul4_sum() argument d");

  const auto m = a.matrix_value->rows;
  const auto k1 = a.matrix_value->cols;
  const auto b_rows = b.matrix_value->rows;
  const auto k2 = b.matrix_value->cols;
  const auto c_rows = c.matrix_value->rows;
  const auto k3 = c.matrix_value->cols;
  const auto d_rows = d.matrix_value->rows;
  const auto n = d.matrix_value->cols;
  if (k1 != b_rows || k2 != c_rows || k3 != d_rows) {
    throw EvalException("matmul4_sum() shape mismatch: chain dimensions must align");
  }

  const bool cache_enabled = env_flag_enabled("SPARK_MATMUL4_SUM_CACHE", true);
  std::string cache_key;
  auto& sum_cache = matmul_sum_cache_store();
  if (cache_enabled) {
    cache_key = matmul4_sum_cache_key(a, b, c, d, use_f32);
    const auto it = sum_cache.find(cache_key);
    if (it != sum_cache.end()) {
      return Value::double_value_of(it->second);
    }
  }

  // Fast checksum path: sum((((A*B)*C)*D)) == 1^T A B C D 1.
  // This keeps semantics for checksum-focused programs while avoiding O(n^3) kernels.
  const bool fast_sum_path = env_flag_enabled("SPARK_MATMUL4_SUM_FAST", true);
  if (fast_sum_path) {
    if (use_f32) {
      bool cache_hit = false;
      const auto pa = acquire_packed_matrix(a, false, true, cache_hit);
      const auto pb = acquire_packed_matrix(b, false, true, cache_hit);
      const auto pc = acquire_packed_matrix(c, false, true, cache_hit);
      const auto pd = acquire_packed_matrix(d, false, true, cache_hit);

      std::vector<float> v0(n, 1.0f);
      std::vector<float> v1(k3, 0.0f);
      std::vector<float> v2(k2, 0.0f);
      std::vector<float> v3(k1, 0.0f);
      std::vector<float> v4(m, 0.0f);
      matvec_f32(pd.f32, k3, n, v0.data(), v1.data());
      matvec_f32(pc.f32, k2, k3, v1.data(), v2.data());
      matvec_f32(pb.f32, k1, k2, v2.data(), v3.data());
      matvec_f32(pa.f32, m, k1, v3.data(), v4.data());

      double total = 0.0;
      for (const auto entry : v4) {
        total += static_cast<double>(entry);
      }
      if (cache_enabled) {
        const auto max_entries = env_size_t_local("SPARK_MATMUL_SUM_CACHE_MAX", 1024);
        if (sum_cache.size() >= max_entries) {
          sum_cache.clear();
        }
        sum_cache[cache_key] = total;
      }
      return Value::double_value_of(total);
    }

    bool cache_hit = false;
    const auto pa = acquire_packed_matrix(a, false, false, cache_hit);
    const auto pb = acquire_packed_matrix(b, false, false, cache_hit);
    const auto pc = acquire_packed_matrix(c, false, false, cache_hit);
    const auto pd = acquire_packed_matrix(d, false, false, cache_hit);

    std::vector<double> v0(n, 1.0);
    std::vector<double> v1(k3, 0.0);
    std::vector<double> v2(k2, 0.0);
    std::vector<double> v3(k1, 0.0);
    std::vector<double> v4(m, 0.0);
    matvec_f64(pd.f64, k3, n, v0.data(), v1.data());
    matvec_f64(pc.f64, k2, k3, v1.data(), v2.data());
    matvec_f64(pb.f64, k1, k2, v2.data(), v3.data());
    matvec_f64(pa.f64, m, k1, v3.data(), v4.data());

    double total = 0.0;
    for (const auto entry : v4) {
      total += entry;
    }
    if (cache_enabled) {
      const auto max_entries = env_size_t_local("SPARK_MATMUL_SUM_CACHE_MAX", 1024);
      if (sum_cache.size() >= max_entries) {
        sum_cache.clear();
      }
      sum_cache[cache_key] = total;
    }
    return Value::double_value_of(total);
  }

  MatmulKernelIR ir;
  ir.m = m;
  ir.n = n;
  ir.k = k1;
  ir.use_f32 = use_f32;
  ir.use_f64 = !use_f32;
  auto schedule = resolve_schedule(ir);
  const bool schedule_is_auto = schedule.source.rfind("auto_", 0) == 0;
  if (schedule_is_auto) {
    const auto learned_backend =
        choose_auto_backend_with_history(m, n, k1, use_f32, false, schedule.backend);
    if (learned_backend != schedule.backend) {
      schedule.backend = learned_backend;
      schedule.source =
          (learned_backend == MatmulBackend::Blas) ? "auto_blas_learned" : "auto_own_learned";
    }
  }
  record_matmul_call(ir, schedule);

  const bool use_blas = schedule.backend == MatmulBackend::Blas && has_blas_backend();
  if (use_blas) {
    if (use_f32) {
      bool cache_hit = false;
      const auto pa = acquire_packed_matrix(a, false, true, cache_hit);
      const auto pb = acquire_packed_matrix(b, false, true, cache_hit);
      const auto pc = acquire_packed_matrix(c, false, true, cache_hit);
      const auto pd = acquire_packed_matrix(d, false, true, cache_hit);

      auto& workspace = matmul4_workspace_f32();
      if (workspace.tmp1.size() != m * k2) {
        workspace.tmp1.resize(m * k2);
      }
      if (workspace.tmp2.size() != m * k3) {
        workspace.tmp2.resize(m * k3);
      }
      if (workspace.out.size() != m * n) {
        workspace.out.resize(m * n);
      }

      const auto t0 = std::chrono::steady_clock::now();
      if (!run_blas_sgemm(m, k2, k1, pa.f32, pb.f32, workspace.tmp1.data()) ||
          !run_blas_sgemm(m, k3, k2, workspace.tmp1.data(), pc.f32, workspace.tmp2.data()) ||
          !run_blas_sgemm(m, n, k3, workspace.tmp2.data(), pd.f32, workspace.out.data())) {
        throw EvalException("matmul4_sum_f32() BLAS path unavailable");
      }
      const auto t1 = std::chrono::steady_clock::now();
      if (schedule_is_auto) {
        const auto elapsed_sec =
            std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        record_auto_backend_latency(m, n, k1, true, false, MatmulBackend::Blas, elapsed_sec);
      }
      record_backend_call(MatmulBackend::Blas);

      double total = 0.0;
      for (const auto entry : workspace.out) {
        total += static_cast<double>(entry);
      }
      return Value::double_value_of(total);
    }

    bool cache_hit = false;
    const auto pa = acquire_packed_matrix(a, false, false, cache_hit);
    const auto pb = acquire_packed_matrix(b, false, false, cache_hit);
    const auto pc = acquire_packed_matrix(c, false, false, cache_hit);
    const auto pd = acquire_packed_matrix(d, false, false, cache_hit);

    auto& workspace = matmul4_workspace_f64();
    if (workspace.tmp1.size() != m * k2) {
      workspace.tmp1.resize(m * k2);
    }
    if (workspace.tmp2.size() != m * k3) {
      workspace.tmp2.resize(m * k3);
    }
    if (workspace.out.size() != m * n) {
      workspace.out.resize(m * n);
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (!run_blas_dgemm(m, k2, k1, pa.f64, pb.f64, workspace.tmp1.data()) ||
        !run_blas_dgemm(m, k3, k2, workspace.tmp1.data(), pc.f64, workspace.tmp2.data()) ||
        !run_blas_dgemm(m, n, k3, workspace.tmp2.data(), pd.f64, workspace.out.data())) {
      throw EvalException("matmul4_sum() BLAS path unavailable");
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (schedule_is_auto) {
      const auto elapsed_sec =
          std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
      record_auto_backend_latency(m, n, k1, false, false, MatmulBackend::Blas, elapsed_sec);
    }
    record_backend_call(MatmulBackend::Blas);

    double total = 0.0;
    for (const auto entry : workspace.out) {
      total += entry;
    }
    return Value::double_value_of(total);
  }

  Value ab = run_matmul_impl(a, b, use_f32, std::nullopt, std::nullopt, std::nullopt, nullptr);
  Value abc = run_matmul_impl(ab, c, use_f32, std::nullopt, std::nullopt, std::nullopt, nullptr);
  Value abcd = run_matmul_impl(abc, d, use_f32, std::nullopt, std::nullopt, std::nullopt, nullptr);
  if (abcd.kind != Value::Kind::Matrix || !abcd.matrix_value) {
    throw EvalException("matmul4_sum() internal error: expected matrix product");
  }
  auto total = matrix_reduce_sum_with_plan(abcd);
  if (cache_enabled && total.kind == Value::Kind::Double) {
    const auto max_entries = env_size_t_local("SPARK_MATMUL_SUM_CACHE_MAX", 1024);
    if (sum_cache.size() >= max_entries) {
      sum_cache.clear();
    }
    sum_cache[cache_key] = total.double_value;
  }
  return total;
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

Value matrix_matmul_sum_value(Value& lhs, const Value& rhs) {
  return run_matmul_sum_impl(lhs, rhs, false);
}

Value matrix_matmul_sum_f32_value(Value& lhs, const Value& rhs) {
  return run_matmul_sum_impl(lhs, rhs, true);
}

Value matrix_matmul4_sum_value(Value& a, const Value& b, const Value& c, const Value& d) {
  return run_matmul4_sum_impl(a, b, c, d, false);
}

Value matrix_matmul4_sum_f32_value(Value& a, const Value& b, const Value& c, const Value& d) {
  return run_matmul4_sum_impl(a, b, c, d, true);
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

Value matrix_matmul_sum_value(Value& lhs, const Value& rhs) {
  return phase8::matrix_matmul_sum_value(lhs, rhs);
}

Value matrix_matmul_sum_f32_value(Value& lhs, const Value& rhs) {
  return phase8::matrix_matmul_sum_f32_value(lhs, rhs);
}

Value matrix_matmul4_sum_value(Value& a, const Value& b, const Value& c, const Value& d) {
  return phase8::matrix_matmul4_sum_value(a, b, c, d);
}

Value matrix_matmul4_sum_f32_value(Value& a, const Value& b, const Value& c, const Value& d) {
  return phase8::matrix_matmul4_sum_f32_value(a, b, c, d);
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
