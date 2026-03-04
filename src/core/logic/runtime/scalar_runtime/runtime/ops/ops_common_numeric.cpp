#include "scalar_runtime/runtime/ops/runtime_ops.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(__arm__) || defined(__thumb__)
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__ARM_FEATURE_NEON)
#include <arm_neon.h>
#endif
#endif

#include "semantic_runtime/evaluator_parts/internal_helpers.h"

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
  // Delegate to shared parser so all runtime flags interpret values identically.
  return env_flag_enabled(name, fallback);
}

namespace {

bool simd_allowed_runtime() {
  static const bool allowed = []() {
    if (!env_bool_enabled("SPARK_SIMD_ENABLE", true)) {
      return false;
    }
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
#elif defined(__aarch64__)
    // Advanced SIMD (NEON) is mandatory in AArch64.
    return true;
#elif defined(__arm__) || defined(__thumb__)
    // Optional NEON on AArch32; enable only when build target has NEON.
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__ARM_FEATURE_NEON)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
  }();
  return allowed;
}

std::size_t simd_min_count_runtime() {
  static const std::size_t threshold = []() -> std::size_t {
    // Adaptive default: avoid SIMD setup cost on small loops.
    // This default targets dense list/matrix kernels, not scalar per-op loops.
    std::size_t fallback = 8192;
#if defined(__aarch64__)
    // Apple/ARM scalar auto-vectorization can beat manual NEON for medium sizes.
    // Keep a high default and allow forcing lower via SPARK_SIMD_MIN_COUNT.
    fallback = 8'388'608;
#elif defined(__arm__) || defined(__thumb__)
    // Scalar fallback on many 32-bit ARM toolchains is efficient; keep a high
    // threshold so NEON dispatch activates only when clearly beneficial.
    fallback = 16'777'216;
#endif
    if (const auto* raw = std::getenv("SPARK_SIMD_MIN_COUNT")) {
      const auto parsed = std::strtoull(raw, nullptr, 10);
      if (parsed > 0) {
        fallback = static_cast<std::size_t>(parsed);
      }
    }
    return fallback;
  }();
  return threshold;
}

}  // namespace

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2"))) void simd_apply_binary_f64_avx2_impl(BinaryOp op,
                                                                      const double* lhs,
                                                                      const double* rhs,
                                                                      double* out,
                                                                      std::size_t count) {
  std::size_t i = 0;
  const std::size_t vec_end = count & ~static_cast<std::size_t>(3);
  for (; i < vec_end; i += 4) {
    const __m256d a = _mm256_loadu_pd(lhs + i);
    const __m256d b = _mm256_loadu_pd(rhs + i);
    __m256d r;
    switch (op) {
      case BinaryOp::Add:
        r = _mm256_add_pd(a, b);
        break;
      case BinaryOp::Sub:
        r = _mm256_sub_pd(a, b);
        break;
      case BinaryOp::Mul:
        r = _mm256_mul_pd(a, b);
        break;
      case BinaryOp::Div:
        r = _mm256_div_pd(a, b);
        break;
      default:
        return;
    }
    _mm256_storeu_pd(out + i, r);
  }
  for (; i < count; ++i) {
    switch (op) {
      case BinaryOp::Add:
        out[i] = lhs[i] + rhs[i];
        break;
      case BinaryOp::Sub:
        out[i] = lhs[i] - rhs[i];
        break;
      case BinaryOp::Mul:
        out[i] = lhs[i] * rhs[i];
        break;
      case BinaryOp::Div:
        out[i] = lhs[i] / rhs[i];
        break;
      default:
        return;
    }
  }
}

__attribute__((target("avx2"))) void simd_apply_binary_f64_scalar_avx2_impl(BinaryOp op,
                                                                             const double* values,
                                                                             double scalar,
                                                                             double* out,
                                                                             std::size_t count,
                                                                             bool values_on_left) {
  std::size_t i = 0;
  const std::size_t vec_end = count & ~static_cast<std::size_t>(3);
  const __m256d s = _mm256_set1_pd(scalar);
  for (; i < vec_end; i += 4) {
    const __m256d v = _mm256_loadu_pd(values + i);
    __m256d r;
    switch (op) {
      case BinaryOp::Add:
        r = _mm256_add_pd(v, s);
        break;
      case BinaryOp::Sub:
        r = values_on_left ? _mm256_sub_pd(v, s) : _mm256_sub_pd(s, v);
        break;
      case BinaryOp::Mul:
        r = _mm256_mul_pd(v, s);
        break;
      case BinaryOp::Div:
        r = values_on_left ? _mm256_div_pd(v, s) : _mm256_div_pd(s, v);
        break;
      default:
        return;
    }
    _mm256_storeu_pd(out + i, r);
  }
  for (; i < count; ++i) {
    const auto lhs = values[i];
    switch (op) {
      case BinaryOp::Add:
        out[i] = lhs + scalar;
        break;
      case BinaryOp::Sub:
        out[i] = values_on_left ? (lhs - scalar) : (scalar - lhs);
        break;
      case BinaryOp::Mul:
        out[i] = lhs * scalar;
        break;
      case BinaryOp::Div:
        out[i] = values_on_left ? (lhs / scalar) : (scalar / lhs);
        break;
      default:
        return;
    }
  }
}
#endif
#endif

bool simd_apply_binary_f64(BinaryOp op, const double* lhs, const double* rhs, double* out, std::size_t count) {
  if (!lhs || !rhs || !out || count < simd_min_count_runtime() || !simd_allowed_runtime()) {
    return false;
  }
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
  if (op != BinaryOp::Add && op != BinaryOp::Sub && op != BinaryOp::Mul) {
    return false;
  }
  const auto has_avx2 = __builtin_cpu_supports("avx2");
  if (!has_avx2) {
    return false;
  }
  // Compile AVX2 kernel irrespective of global target flags.
  simd_apply_binary_f64_avx2_impl(op, lhs, rhs, out, count);
  return true;
#else
  (void)op;
  (void)lhs;
  (void)rhs;
  (void)out;
  (void)count;
  return false;
#endif
#elif defined(__aarch64__) || ((defined(__arm__) || defined(__thumb__)) && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__ARM_FEATURE_NEON)))
  if (op != BinaryOp::Add && op != BinaryOp::Sub && op != BinaryOp::Mul && op != BinaryOp::Div) {
    return false;
  }
  std::size_t i = 0;
  const std::size_t vec_end = count & ~static_cast<std::size_t>(1);
  for (; i < vec_end; i += 2) {
    const float64x2_t a = vld1q_f64(lhs + i);
    const float64x2_t b = vld1q_f64(rhs + i);
    float64x2_t r;
    switch (op) {
      case BinaryOp::Add:
        r = vaddq_f64(a, b);
        break;
      case BinaryOp::Sub:
        r = vsubq_f64(a, b);
        break;
      case BinaryOp::Mul:
        r = vmulq_f64(a, b);
        break;
      default:
        return false;
    }
    vst1q_f64(out + i, r);
  }
  for (; i < count; ++i) {
    switch (op) {
      case BinaryOp::Add:
        out[i] = lhs[i] + rhs[i];
        break;
      case BinaryOp::Sub:
        out[i] = lhs[i] - rhs[i];
        break;
      case BinaryOp::Mul:
        out[i] = lhs[i] * rhs[i];
        break;
      default:
        return false;
    }
  }
  return true;
#else
  (void)op;
  (void)lhs;
  (void)rhs;
  (void)out;
  (void)count;
  return false;
#endif
}

bool simd_apply_binary_f64_scalar(BinaryOp op, const double* values, double scalar, double* out,
                                  std::size_t count, bool values_on_left) {
  if (!values || !out || count < simd_min_count_runtime() || !simd_allowed_runtime()) {
    return false;
  }
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
  if (op != BinaryOp::Add && op != BinaryOp::Sub && op != BinaryOp::Mul) {
    return false;
  }
  const auto has_avx2 = __builtin_cpu_supports("avx2");
  if (!has_avx2) {
    return false;
  }
  simd_apply_binary_f64_scalar_avx2_impl(op, values, scalar, out, count, values_on_left);
  return true;
#else
  (void)op;
  (void)values;
  (void)scalar;
  (void)out;
  (void)count;
  (void)values_on_left;
  return false;
#endif
#elif defined(__aarch64__) || ((defined(__arm__) || defined(__thumb__)) && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__ARM_FEATURE_NEON)))
  if (op != BinaryOp::Add && op != BinaryOp::Sub && op != BinaryOp::Mul && op != BinaryOp::Div) {
    return false;
  }
  std::size_t i = 0;
  const std::size_t vec_end = count & ~static_cast<std::size_t>(1);
  const float64x2_t s = vdupq_n_f64(scalar);
  for (; i < vec_end; i += 2) {
    const float64x2_t v = vld1q_f64(values + i);
    float64x2_t r;
    switch (op) {
      case BinaryOp::Add:
        r = vaddq_f64(v, s);
        break;
      case BinaryOp::Sub:
        r = values_on_left ? vsubq_f64(v, s) : vsubq_f64(s, v);
        break;
      case BinaryOp::Mul:
        r = vmulq_f64(v, s);
        break;
      default:
        return false;
    }
    vst1q_f64(out + i, r);
  }
  for (; i < count; ++i) {
    const auto lhs = values[i];
    switch (op) {
      case BinaryOp::Add:
        out[i] = lhs + scalar;
        break;
      case BinaryOp::Sub:
        out[i] = values_on_left ? (lhs - scalar) : (scalar - lhs);
        break;
      case BinaryOp::Mul:
        out[i] = lhs * scalar;
        break;
      default:
        return false;
    }
  }
  return true;
#else
  (void)op;
  (void)values;
  (void)scalar;
  (void)out;
  (void)count;
  (void)values_on_left;
  return false;
#endif
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
