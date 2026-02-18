#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase8 {

namespace {

struct MatrixProbe {
  bool valid = false;
  double first = 0.0;
  double middle = 0.0;
  double last = 0.0;
};

struct PackCacheEntry {
  std::size_t rows = 0;
  std::size_t cols = 0;
  bool transpose = false;
  bool use_f32 = false;
  std::vector<double> packed_f64;
  std::vector<float> packed_f32;
  MatrixProbe probe;
};

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

}  // namespace

struct PackedMatrixView {
  const double* f64 = nullptr;
  const float* f32 = nullptr;
  std::size_t rows = 0;
  std::size_t cols = 0;
  bool use_f32 = false;
};

struct MatmulRunStats {
  std::size_t calls = 0;
  std::size_t own_calls = 0;
  std::size_t blas_calls = 0;
  std::size_t pack_a_count = 0;
  std::size_t pack_b_count = 0;
  std::size_t cache_hit_a = 0;
  std::size_t cache_hit_b = 0;
  std::size_t epilogue_fused_calls = 0;
  std::size_t last_m = 0;
  std::size_t last_n = 0;
  std::size_t last_k = 0;
  MatmulBackend last_backend = MatmulBackend::Own;
  MatmulSchedule last_schedule = {};
  std::string last_schedule_source = "default";
};

struct AutoBackendPerf {
  double own_avg_sec = 0.0;
  double blas_avg_sec = 0.0;
  std::size_t own_samples = 0;
  std::size_t blas_samples = 0;
};

bool auto_env_bool_enabled(const char* name, bool fallback) {
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

std::size_t auto_env_size_t(const char* name, std::size_t fallback) {
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

double auto_env_double(const char* name, double fallback) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const auto parsed = std::strtod(value, &end);
  if (end == value) {
    return fallback;
  }
  return parsed;
}

bool auto_backend_learning_enabled() {
  return auto_env_bool_enabled("SPARK_MATMUL_AUTO_LEARN", true);
}

std::string auto_backend_key(std::size_t m, std::size_t n, std::size_t k, bool use_f32,
                             bool has_epilogue) {
  std::ostringstream out;
  out << m << "x" << n << "x" << k << ":" << (use_f32 ? "f32" : "f64") << ":"
      << (has_epilogue ? "epi" : "core");
  return out.str();
}

std::unordered_map<std::string, AutoBackendPerf>& auto_backend_perf_store() {
  static std::unordered_map<std::string, AutoBackendPerf> store;
  return store;
}

std::mutex& auto_backend_perf_mutex() {
  static std::mutex mutex;
  return mutex;
}

void update_ema(double& avg, std::size_t& samples, double value) {
  constexpr double kAlpha = 0.25;
  if (samples == 0) {
    avg = value;
  } else {
    avg = (1.0 - kAlpha) * avg + kAlpha * value;
  }
  samples += 1;
}

bool matrix_numeric_cell(const Value& value) {
  return value.kind == Value::Kind::Int || value.kind == Value::Kind::Double;
}

double matrix_cell_to_f64(const Value& value) {
  if (value.kind == Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == Value::Kind::Double) {
    return value.double_value;
  }
  throw EvalException("matmul() expects numeric matrix cells");
}

float matrix_cell_to_f32(const Value& value) {
  return static_cast<float>(matrix_cell_to_f64(value));
}

MatrixProbe matrix_probe(const Value& matrix) {
  MatrixProbe probe;
  if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
    return probe;
  }

  if (const auto* dense = dense_f64_if_materialized(matrix)) {
    if (!dense->empty()) {
      probe.valid = true;
      probe.first = dense->front();
      probe.middle = (*dense)[dense->size() / 2];
      probe.last = dense->back();
      return probe;
    }
  }

  const auto& data = matrix.matrix_value->data;
  if (data.empty()) {
    return probe;
  }
  probe.valid = true;
  probe.first = matrix_cell_to_f64(data.front());
  probe.middle = matrix_cell_to_f64(data[data.size() / 2]);
  probe.last = matrix_cell_to_f64(data.back());
  return probe;
}

bool same_probe(const MatrixProbe& lhs, const MatrixProbe& rhs) {
  if (lhs.valid != rhs.valid) {
    return false;
  }
  if (!lhs.valid) {
    return true;
  }
  constexpr double kTol = 1e-12;
  return std::fabs(lhs.first - rhs.first) <= kTol &&
         std::fabs(lhs.middle - rhs.middle) <= kTol &&
         std::fabs(lhs.last - rhs.last) <= kTol;
}

std::string pack_key(const Value& matrix, bool transpose, bool use_f32) {
  if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
    return "invalid";
  }
  std::ostringstream out;
  out << reinterpret_cast<std::uintptr_t>(matrix.matrix_value.get()) << ":"
      << matrix.matrix_value->rows << "x" << matrix.matrix_value->cols << ":v"
      << matrix.matrix_cache.version << ":" << (transpose ? "t" : "n")
      << ":" << (use_f32 ? "f32" : "f64");
  return out.str();
}

std::unordered_map<std::string, PackCacheEntry>& pack_cache_store() {
  static std::unordered_map<std::string, PackCacheEntry> cache;
  return cache;
}

MatmulRunStats& mutable_matmul_stats() {
  static MatmulRunStats stats;
  return stats;
}

PackedMatrixView acquire_packed_matrix(const Value& matrix, bool transpose, bool use_f32,
                                       bool& cache_hit) {
  if (matrix.kind != Value::Kind::Matrix || !matrix.matrix_value) {
    throw EvalException("matmul() expects matrix receiver");
  }

  if (!transpose && !use_f32) {
    if (const auto* dense = dense_f64_if_materialized(matrix)) {
      cache_hit = true;
      PackedMatrixView view;
      view.rows = matrix.matrix_value->rows;
      view.cols = matrix.matrix_value->cols;
      view.use_f32 = false;
      view.f64 = dense->data();
      view.f32 = nullptr;
      return view;
    }
  }

  const auto key = pack_key(matrix, transpose, use_f32);
  auto& cache = pack_cache_store();
  auto probe = matrix_probe(matrix);

  auto it = cache.find(key);
  if (it != cache.end() && same_probe(it->second.probe, probe)) {
    cache_hit = true;
    PackedMatrixView view;
    view.rows = it->second.rows;
    view.cols = it->second.cols;
    view.use_f32 = use_f32;
    view.f64 = it->second.packed_f64.empty() ? nullptr : it->second.packed_f64.data();
    view.f32 = it->second.packed_f32.empty() ? nullptr : it->second.packed_f32.data();
    return view;
  }

  cache_hit = false;
  PackCacheEntry entry;
  entry.rows = transpose ? matrix.matrix_value->cols : matrix.matrix_value->rows;
  entry.cols = transpose ? matrix.matrix_value->rows : matrix.matrix_value->cols;
  entry.transpose = transpose;
  entry.use_f32 = use_f32;
  entry.probe = probe;

  const auto rows = matrix.matrix_value->rows;
  const auto cols = matrix.matrix_value->cols;
  const auto& data = matrix.matrix_value->data;
  const auto total = rows * cols;
  const auto* dense_f64 = dense_f64_if_materialized(matrix);
  if (!dense_f64 && data.size() != total) {
    throw EvalException("matmul() matrix payload is inconsistent with shape");
  }

  if (use_f32) {
    entry.packed_f32.resize(total);
  } else {
    entry.packed_f64.resize(total);
  }

  if (dense_f64) {
    for (std::size_t r = 0; r < rows; ++r) {
      for (std::size_t c = 0; c < cols; ++c) {
        const auto input_index = r * cols + c;
        const auto output_index = transpose ? (c * rows + r) : input_index;
        if (use_f32) {
          entry.packed_f32[output_index] = static_cast<float>((*dense_f64)[input_index]);
        } else {
          entry.packed_f64[output_index] = (*dense_f64)[input_index];
        }
      }
    }
  } else {
    for (std::size_t r = 0; r < rows; ++r) {
      for (std::size_t c = 0; c < cols; ++c) {
        const auto input_index = r * cols + c;
        const auto output_index = transpose ? (c * rows + r) : input_index;
        const auto& value = data[input_index];
        if (!matrix_numeric_cell(value)) {
          throw EvalException("matmul() requires numeric matrix cells");
        }
        if (use_f32) {
          entry.packed_f32[output_index] = matrix_cell_to_f32(value);
        } else {
          entry.packed_f64[output_index] = matrix_cell_to_f64(value);
        }
      }
    }
  }

  auto [inserted_it, _] = cache.insert_or_assign(key, std::move(entry));
  PackedMatrixView view;
  view.rows = inserted_it->second.rows;
  view.cols = inserted_it->second.cols;
  view.use_f32 = use_f32;
  view.f64 = inserted_it->second.packed_f64.empty() ? nullptr : inserted_it->second.packed_f64.data();
  view.f32 = inserted_it->second.packed_f32.empty() ? nullptr : inserted_it->second.packed_f32.data();
  return view;
}

void record_matmul_call(const MatmulKernelIR& ir, const MatmulSchedule& schedule) {
  auto& stats = mutable_matmul_stats();
  stats.calls += 1;
  stats.last_m = ir.m;
  stats.last_n = ir.n;
  stats.last_k = ir.k;
  stats.last_schedule = schedule;
  stats.last_schedule_source = schedule.source;
}

void record_backend_call(const MatmulBackend backend) {
  auto& stats = mutable_matmul_stats();
  stats.last_backend = backend;
  if (backend == MatmulBackend::Blas) {
    stats.blas_calls += 1;
  } else {
    stats.own_calls += 1;
  }
}

void record_pack_event(bool is_a, bool cache_hit) {
  auto& stats = mutable_matmul_stats();
  if (is_a) {
    if (cache_hit) {
      stats.cache_hit_a += 1;
    } else {
      stats.pack_a_count += 1;
    }
    return;
  }
  if (cache_hit) {
    stats.cache_hit_b += 1;
  } else {
    stats.pack_b_count += 1;
  }
}

void record_epilogue_fused() { mutable_matmul_stats().epilogue_fused_calls += 1; }

const MatmulRunStats& matmul_stats_snapshot() { return mutable_matmul_stats(); }

MatmulBackend choose_auto_backend_with_history(std::size_t m, std::size_t n, std::size_t k,
                                               bool use_f32, bool has_epilogue,
                                               MatmulBackend fallback) {
  if (!auto_backend_learning_enabled() || !has_blas_backend()) {
    return fallback;
  }
  // Large dense GEMM should stay on BLAS by default. Learned routing is most
  // valuable in small/medium regimes where dispatch noise can flip winners.
  const auto force_blas_dim = auto_env_size_t("SPARK_MATMUL_AUTO_FORCE_BLAS_DIM", 768);
  if (force_blas_dim > 0 &&
      m >= force_blas_dim && n >= force_blas_dim && k >= force_blas_dim) {
    return MatmulBackend::Blas;
  }

  const auto key = auto_backend_key(m, n, k, use_f32, has_epilogue);
  const auto min_samples = auto_env_size_t("SPARK_MATMUL_AUTO_LEARN_MIN_SAMPLES", 4);
  const auto switch_margin = auto_env_double("SPARK_MATMUL_AUTO_LEARN_MARGIN_SEC", 0.0);

  std::lock_guard<std::mutex> lock(auto_backend_perf_mutex());
  const auto& store = auto_backend_perf_store();
  const auto it = store.find(key);
  if (it == store.end()) {
    return fallback;
  }
  const auto& perf = it->second;
  if (perf.own_samples < min_samples || perf.blas_samples < min_samples) {
    return fallback;
  }
  if (perf.blas_avg_sec + switch_margin < perf.own_avg_sec) {
    return MatmulBackend::Blas;
  }
  if (perf.own_avg_sec + switch_margin < perf.blas_avg_sec) {
    return MatmulBackend::Own;
  }
  return fallback;
}

void record_auto_backend_latency(std::size_t m, std::size_t n, std::size_t k, bool use_f32,
                                 bool has_epilogue, MatmulBackend backend, double elapsed_sec) {
  if (!auto_backend_learning_enabled() || elapsed_sec <= 0.0) {
    return;
  }
  const auto key = auto_backend_key(m, n, k, use_f32, has_epilogue);
  std::lock_guard<std::mutex> lock(auto_backend_perf_mutex());
  auto& perf = auto_backend_perf_store()[key];
  if (backend == MatmulBackend::Blas) {
    update_ema(perf.blas_avg_sec, perf.blas_samples, elapsed_sec);
  } else {
    update_ema(perf.own_avg_sec, perf.own_samples, elapsed_sec);
  }
}

}  // namespace phase8
}  // namespace spark
