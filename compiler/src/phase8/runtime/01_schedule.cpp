#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "../../phase3/evaluator_parts/internal_helpers.h"
#include "spark/cpu_features.h"

namespace spark {
namespace phase8 {

enum class MatmulBackend {
  Own = 0,
  Blas = 1,
};

struct MatmulKernelIR {
  std::size_t m = 0;
  std::size_t n = 0;
  std::size_t k = 0;
  bool use_f32 = false;
  bool use_f64 = true;
};

struct MatmulSchedule {
  MatmulBackend backend = MatmulBackend::Own;
  std::size_t tile_m = 64;
  std::size_t tile_n = 64;
  std::size_t tile_k = 64;
  std::size_t unroll = 4;
  std::size_t vector_width = 4;
  bool pack_a = true;
  bool pack_b = true;
  std::string source = "default";
};

using CblasDgemmFn = void (*)(int, int, int, int, int, int, double, const double*, int,
                              const double*, int, double, double*, int);
using CblasSgemmFn = void (*)(int, int, int, int, int, int, float, const float*, int,
                              const float*, int, float, float*, int);

struct BlasSymbols {
  void* handle = nullptr;
  CblasDgemmFn dgemm = nullptr;
  CblasSgemmFn sgemm = nullptr;
  bool ready = false;
};

struct TunedFileCache {
  bool initialized = false;
  bool enabled = true;
  std::string path;
  bool has_schedule = false;
  MatmulSchedule schedule = {};
};

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

std::optional<std::size_t> env_size(const char* name) {
  const auto* value = std::getenv(name);
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  const auto parsed = std::strtoull(value, nullptr, 10);
  if (parsed == 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(parsed);
}

std::size_t saturating_volume3(std::size_t a, std::size_t b, std::size_t c) {
  constexpr auto kMax = std::numeric_limits<std::size_t>::max();
  if (a == 0 || b == 0 || c == 0) {
    return 0;
  }
  if (a > kMax / b) {
    return kMax;
  }
  const auto ab = a * b;
  if (ab > kMax / c) {
    return kMax;
  }
  return ab * c;
}

BlasSymbols& blas_symbols() {
  static BlasSymbols symbols = [] {
    BlasSymbols out;
    const std::vector<const char*> libs = {
        "/System/Library/Frameworks/Accelerate.framework/Accelerate",
        "libopenblas.dylib",
        "libopenblas.so",
        "libblas.dylib",
        "libblas.so.3",
        "libblas.so",
    };
    for (const auto* lib : libs) {
      auto* handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL);
      if (!handle) {
        continue;
      }
      auto dgemm = reinterpret_cast<CblasDgemmFn>(dlsym(handle, "cblas_dgemm"));
      auto sgemm = reinterpret_cast<CblasSgemmFn>(dlsym(handle, "cblas_sgemm"));
      if (!dgemm || !sgemm) {
        dlclose(handle);
        continue;
      }
      out.handle = handle;
      out.dgemm = dgemm;
      out.sgemm = sgemm;
      out.ready = true;
      return out;
    }
    return out;
  }();
  return symbols;
}

bool has_blas_backend() { return blas_symbols().ready; }

bool run_blas_dgemm(std::size_t m, std::size_t n, std::size_t k,
                    const double* a, const double* b, double* c) {
  const auto& symbols = blas_symbols();
  if (!symbols.ready || !symbols.dgemm || !a || !b || !c) {
    return false;
  }
  constexpr int kCblasRowMajor = 101;
  constexpr int kCblasNoTrans = 111;
  symbols.dgemm(kCblasRowMajor, kCblasNoTrans, kCblasNoTrans,
                static_cast<int>(m), static_cast<int>(n), static_cast<int>(k),
                1.0, a, static_cast<int>(k),
                b, static_cast<int>(n),
                0.0, c, static_cast<int>(n));
  return true;
}

bool run_blas_sgemm(std::size_t m, std::size_t n, std::size_t k,
                    const float* a, const float* b, float* c) {
  const auto& symbols = blas_symbols();
  if (!symbols.ready || !symbols.sgemm || !a || !b || !c) {
    return false;
  }
  constexpr int kCblasRowMajor = 101;
  constexpr int kCblasNoTrans = 111;
  symbols.sgemm(kCblasRowMajor, kCblasNoTrans, kCblasNoTrans,
                static_cast<int>(m), static_cast<int>(n), static_cast<int>(k),
                1.0f, a, static_cast<int>(k),
                b, static_cast<int>(n),
                0.0f, c, static_cast<int>(n));
  return true;
}

bool parse_tuned_file(const std::string& path, MatmulSchedule& schedule) {
  std::ifstream file(path);
  if (!file) {
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (text.empty()) {
    return false;
  }

  const auto extract_number = [&text](const char* key) -> std::optional<std::size_t> {
    const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(text, match, pattern) || match.size() < 2) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(std::strtoull(match[1].str().c_str(), nullptr, 10));
  };
  const auto extract_string = [&text](const char* key) -> std::string {
    const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch match;
    if (!std::regex_search(text, match, pattern) || match.size() < 2) {
      return "";
    }
    return match[1].str();
  };

  if (const auto v = extract_number("tile_m")) {
    schedule.tile_m = *v;
  }
  if (const auto v = extract_number("tile_n")) {
    schedule.tile_n = *v;
  }
  if (const auto v = extract_number("tile_k")) {
    schedule.tile_k = *v;
  }
  if (const auto v = extract_number("unroll")) {
    schedule.unroll = *v;
  }
  if (const auto v = extract_number("vector_width")) {
    schedule.vector_width = *v;
  }
  const auto backend = extract_string("backend");
  if (backend == "blas") {
    schedule.backend = MatmulBackend::Blas;
  } else if (backend == "own") {
    schedule.backend = MatmulBackend::Own;
  }
  schedule.source = "tuned_file";
  return true;
}

TunedFileCache& tuned_file_cache() {
  static TunedFileCache cache;
  return cache;
}

void maybe_apply_tuned_file(MatmulSchedule& schedule) {
  const bool use_tuned = env_bool_enabled("SPARK_MATMUL_USE_TUNED", true);
  std::string path = "bench/results/matmul_tuned_schedule.json";
  if (const auto* env_path = std::getenv("SPARK_MATMUL_TUNED_CONFIG")) {
    if (*env_path != '\0') {
      path = env_path;
    }
  }

  auto& cache = tuned_file_cache();
  if (!cache.initialized || cache.enabled != use_tuned || cache.path != path) {
    cache = TunedFileCache{};
    cache.initialized = true;
    cache.enabled = use_tuned;
    cache.path = path;
    if (use_tuned) {
      MatmulSchedule parsed = schedule;
      cache.has_schedule = parse_tuned_file(path, parsed);
      if (cache.has_schedule) {
        cache.schedule = parsed;
      }
    }
  }

  if (!cache.enabled || !cache.has_schedule) {
    return;
  }

  schedule.tile_m = cache.schedule.tile_m;
  schedule.tile_n = cache.schedule.tile_n;
  schedule.tile_k = cache.schedule.tile_k;
  schedule.unroll = cache.schedule.unroll;
  schedule.vector_width = cache.schedule.vector_width;
  schedule.backend = cache.schedule.backend;
  schedule.source = cache.schedule.source;
}

MatmulSchedule resolve_schedule(const MatmulKernelIR& ir) {
  MatmulSchedule schedule;
  const auto variant = spark::phase8_matmul_variant_tag(ir.use_f32);
  schedule.vector_width = spark::phase8_recommended_vector_width(ir.use_f32);
  schedule.source = "cpu_dispatch:" + variant;
  const auto max_dim = std::max({ir.m, ir.n, ir.k});
  if (ir.m >= 512 || ir.n >= 512 || ir.k >= 512) {
    schedule.tile_m = 128;
    schedule.tile_n = 128;
    schedule.tile_k = 128;
    schedule.unroll = 8;
    schedule.vector_width = ir.use_f32 ? 8 : 4;
  }

  maybe_apply_tuned_file(schedule);

  // Keep tuned/default schedule adaptive by size: 128 prefers wider tiles,
  // while 256/512 benefit from shorter cache-friendly tiles.
  if (!env_bool_enabled("SPARK_MATMUL_DISABLE_DIM_ADAPTIVE", false)) {
    if (!ir.use_f32) {
      if (max_dim <= 160) {
        schedule.tile_m = 96;
        schedule.tile_n = 96;
        schedule.tile_k = 96;
      } else if (max_dim >= 224) {
        schedule.tile_m = 64;
        schedule.tile_n = 64;
        schedule.tile_k = 64;
      }
      schedule.unroll = std::max<std::size_t>(schedule.unroll, 8);
    } else if (max_dim >= 256) {
      schedule.tile_m = 96;
      schedule.tile_n = 96;
      schedule.tile_k = 96;
      schedule.unroll = std::max<std::size_t>(schedule.unroll, 8);
    }
  }

  if (const auto v = env_size("SPARK_MATMUL_TILE_M")) schedule.tile_m = *v;
  if (const auto v = env_size("SPARK_MATMUL_TILE_N")) schedule.tile_n = *v;
  if (const auto v = env_size("SPARK_MATMUL_TILE_K")) schedule.tile_k = *v;
  if (const auto v = env_size("SPARK_MATMUL_UNROLL")) schedule.unroll = *v;
  if (const auto v = env_size("SPARK_MATMUL_VECTOR_WIDTH")) schedule.vector_width = *v;

  const std::string backend = std::getenv("SPARK_MATMUL_BACKEND")
                                  ? std::string(std::getenv("SPARK_MATMUL_BACKEND"))
                                  : "auto";
  if (backend == "own") {
    schedule.backend = MatmulBackend::Own;
    schedule.source = "env_own";
  } else if (backend == "blas") {
    schedule.backend = has_blas_backend() ? MatmulBackend::Blas : MatmulBackend::Own;
    schedule.source = has_blas_backend() ? "env_blas" : "env_blas_fallback";
  } else {
    const bool respect_tuned_backend =
        env_bool_enabled("SPARK_MATMUL_AUTO_RESPECT_TUNED_BACKEND", false);
    if (!(respect_tuned_backend && schedule.source == "tuned_file")) {
      const auto dim_threshold = env_size("SPARK_MATMUL_AUTO_DIM_THRESHOLD").value_or(224ull);
      const auto small_dim_blas_threshold =
          env_size("SPARK_MATMUL_AUTO_SMALL_BLAS_DIM_THRESHOLD").value_or(112ull);
      const auto volume_threshold =
          env_size("SPARK_MATMUL_AUTO_VOLUME_THRESHOLD")
              .value_or(224ull * 224ull * 224ull);
      const auto volume = saturating_volume3(ir.m, ir.n, ir.k);
      const bool large_problem = (ir.m >= dim_threshold && ir.n >= dim_threshold &&
                                  ir.k >= dim_threshold) ||
                                 (volume >= volume_threshold);
      const bool small_problem =
          small_dim_blas_threshold > 0 && ir.m <= small_dim_blas_threshold &&
          ir.n <= small_dim_blas_threshold && ir.k <= small_dim_blas_threshold;
      if ((large_problem || small_problem) && has_blas_backend()) {
        schedule.backend = MatmulBackend::Blas;
        schedule.source = large_problem ? "auto_blas_large" : "auto_blas_small";
      } else {
        schedule.backend = MatmulBackend::Own;
        schedule.source = "auto_own";
      }
    }
  }

  schedule.tile_m = std::max<std::size_t>(8, schedule.tile_m);
  schedule.tile_n = std::max<std::size_t>(8, schedule.tile_n);
  schedule.tile_k = std::max<std::size_t>(8, schedule.tile_k);
  schedule.unroll = std::max<std::size_t>(1, schedule.unroll);
  schedule.vector_width = std::max<std::size_t>(1, schedule.vector_width);
  return schedule;
}

}  // namespace phase8
}  // namespace spark
