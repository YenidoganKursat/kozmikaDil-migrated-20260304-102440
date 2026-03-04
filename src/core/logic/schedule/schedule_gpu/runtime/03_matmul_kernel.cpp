#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../../semantic_runtime/evaluator_parts/internal_helpers.h"
#include "spark/cpu_features.h"

namespace spark {
namespace matmul_schedule_gpu {

namespace {

constexpr int kCblasRowMajor = 101;
constexpr int kCblasNoTrans = 111;

struct KernelDispatchConfig {
  std::size_t j8_threshold = 256;
  std::size_t j4_threshold = 192;
  std::size_t default_thread_cap = 4;
  std::size_t min_parallel_volume = 4ull * 1024ull * 1024ull;
};

std::optional<std::size_t> env_size_t(const char* name) {
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

bool strict_fp_enabled() {
  // Global precision policy: strict FP is always enabled.
  // Runtime should never relax numerical semantics via env toggles.
  return true;
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

KernelDispatchConfig resolve_kernel_dispatch_config(bool use_f32) {
  KernelDispatchConfig cfg;
  const auto tag = spark::matmul_variant_tag(use_f32);

  if (tag == "x86_avx512") {
    cfg.j8_threshold = 96;
    cfg.j4_threshold = 64;
    cfg.default_thread_cap = 8;
    cfg.min_parallel_volume = 2ull * 1024ull * 1024ull;
  } else if (tag == "x86_avx2") {
    cfg.j8_threshold = 160;
    cfg.j4_threshold = 112;
    cfg.default_thread_cap = 6;
    cfg.min_parallel_volume = 3ull * 1024ull * 1024ull;
  } else if (tag == "arm_sve2" || tag == "arm_sve") {
    cfg.j8_threshold = 160;
    cfg.j4_threshold = 112;
    cfg.default_thread_cap = 6;
    cfg.min_parallel_volume = 3ull * 1024ull * 1024ull;
  } else if (tag == "arm_neon") {
    // ARM NEON: enable wider inner-loop strips earlier; this improves
    // 100/256/512-size steady-state on current Apple Silicon hosts.
    cfg.j8_threshold = 160;
    cfg.j4_threshold = 96;
    cfg.default_thread_cap = 12;
    cfg.min_parallel_volume = 3ull * 1024ull * 1024ull;
  } else if (tag == "riscv_rvv") {
    cfg.j8_threshold = 224;
    cfg.j4_threshold = 128;
    cfg.default_thread_cap = 4;
    cfg.min_parallel_volume = 3ull * 1024ull * 1024ull;
  } else {
    cfg.j8_threshold = 256;
    cfg.j4_threshold = 192;
    cfg.default_thread_cap = 4;
    cfg.min_parallel_volume = 4ull * 1024ull * 1024ull;
  }

  if (const auto v = env_size_t("SPARK_MATMUL_OWN_J8_THRESHOLD")) {
    cfg.j8_threshold = *v;
  }
  if (const auto v = env_size_t("SPARK_MATMUL_OWN_J4_THRESHOLD")) {
    cfg.j4_threshold = *v;
  }
  if (const auto v = env_size_t("SPARK_MATMUL_OWN_DEFAULT_THREADS")) {
    cfg.default_thread_cap = *v;
  }
  if (const auto v = env_size_t("SPARK_MATMUL_OWN_DEFAULT_MIN_VOLUME")) {
    cfg.min_parallel_volume = *v;
  }
  return cfg;
}

template <typename WorkerFn>
void run_parallel_row_tiles(std::size_t m, std::size_t n, std::size_t k,
                            std::size_t tile_m, std::size_t default_thread_cap,
                            std::size_t default_min_volume, WorkerFn&& worker) {
  const auto tile_count = (m + tile_m - 1) / tile_m;
  if (tile_count <= 1) {
    worker(0, tile_count);
    return;
  }

  const auto volume = saturating_volume3(m, n, k);
  const auto min_volume =
      env_size_t("SPARK_MATMUL_OWN_PAR_MIN_VOLUME").value_or(default_min_volume);
  if (volume < min_volume) {
    worker(0, tile_count);
    return;
  }

  std::size_t thread_cap = env_size_t("SPARK_MATMUL_OWN_THREADS").value_or(0);
  if (thread_cap == 0) {
    const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    thread_cap = (hw == 0) ? 1 : std::min<std::size_t>(hw, std::max<std::size_t>(1, default_thread_cap));
  }
  if (thread_cap <= 1) {
    worker(0, tile_count);
    return;
  }

  const auto workers = std::min<std::size_t>(thread_cap, tile_count);
  if (workers <= 1) {
    worker(0, tile_count);
    return;
  }

  std::mutex error_mutex;
  std::exception_ptr first_error;
  std::vector<std::thread> pool;
  pool.reserve(workers - 1);

  for (std::size_t id = 1; id < workers; ++id) {
    const auto begin = (tile_count * id) / workers;
    const auto end = (tile_count * (id + 1)) / workers;
    pool.emplace_back([&worker, begin, end, &error_mutex, &first_error]() {
      try {
        worker(begin, end);
      } catch (...) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!first_error) {
          first_error = std::current_exception();
        }
      }
    });
  }

  try {
    worker(0, tile_count / workers);
  } catch (...) {
    std::lock_guard<std::mutex> lock(error_mutex);
    if (!first_error) {
      first_error = std::current_exception();
    }
  }

  for (auto& thread : pool) {
    thread.join();
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

void run_own_gemm_f64_strict(const PackedMatrixView& a, const PackedMatrixView& b, bool b_transposed,
                             std::size_t m, std::size_t n, std::size_t k,
                             const MatmulSchedule& schedule, std::vector<double>& out) {
  out.assign(m * n, 0.0);
  const auto tm = std::max<std::size_t>(8, schedule.tile_m);
  const auto* a_data = a.f64;
  const auto* b_data = b.f64;
  if (!a_data || !b_data) {
    throw EvalException("matmul_f64 strict kernel received invalid packed buffers");
  }
  auto* out_data = out.data();
  const auto dispatch_cfg = resolve_kernel_dispatch_config(false);
  const auto j8_threshold = dispatch_cfg.j8_threshold;
  const auto j4_threshold = dispatch_cfg.j4_threshold;
  const auto run_tiles = [&](std::size_t tile_begin, std::size_t tile_end) {
    for (std::size_t tile = tile_begin; tile < tile_end; ++tile) {
      const auto i0 = tile * tm;
      const auto i1 = std::min(i0 + tm, m);
      for (std::size_t i = i0; i < i1; ++i) {
        const auto* a_row = a_data + i * k;
        auto* out_row = out_data + i * n;
        std::size_t j = 0;
        if (b_transposed) {
          if (n >= j8_threshold) {
            for (; j + 7 < n; j += 8) {
              double s0 = 0.0;
              double s1 = 0.0;
              double s2 = 0.0;
              double s3 = 0.0;
              double s4 = 0.0;
              double s5 = 0.0;
              double s6 = 0.0;
              double s7 = 0.0;
              const auto* b0 = b_data + (j + 0) * k;
              const auto* b1 = b_data + (j + 1) * k;
              const auto* b2 = b_data + (j + 2) * k;
              const auto* b3 = b_data + (j + 3) * k;
              const auto* b4 = b_data + (j + 4) * k;
              const auto* b5 = b_data + (j + 5) * k;
              const auto* b6 = b_data + (j + 6) * k;
              const auto* b7 = b_data + (j + 7) * k;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                s0 += av * b0[p];
                s1 += av * b1[p];
                s2 += av * b2[p];
                s3 += av * b3[p];
                s4 += av * b4[p];
                s5 += av * b5[p];
                s6 += av * b6[p];
                s7 += av * b7[p];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
              out_row[j + 4] = s4;
              out_row[j + 5] = s5;
              out_row[j + 6] = s6;
              out_row[j + 7] = s7;
            }
          }
          if (n >= j4_threshold) {
            for (; j + 3 < n; j += 4) {
              double s0 = 0.0;
              double s1 = 0.0;
              double s2 = 0.0;
              double s3 = 0.0;
              const auto* b0 = b_data + (j + 0) * k;
              const auto* b1 = b_data + (j + 1) * k;
              const auto* b2 = b_data + (j + 2) * k;
              const auto* b3 = b_data + (j + 3) * k;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                s0 += av * b0[p];
                s1 += av * b1[p];
                s2 += av * b2[p];
                s3 += av * b3[p];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
            }
          }
          for (; j < n; ++j) {
            double sum = 0.0;
            const auto* b_row = b_data + j * k;
            for (std::size_t p = 0; p < k; ++p) {
              sum += a_row[p] * b_row[p];
            }
            out_row[j] = sum;
          }
        } else {
          if (n >= j8_threshold) {
            for (; j + 7 < n; j += 8) {
              double s0 = 0.0;
              double s1 = 0.0;
              double s2 = 0.0;
              double s3 = 0.0;
              double s4 = 0.0;
              double s5 = 0.0;
              double s6 = 0.0;
              double s7 = 0.0;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                const auto base = p * n + j;
                s0 += av * b_data[base + 0];
                s1 += av * b_data[base + 1];
                s2 += av * b_data[base + 2];
                s3 += av * b_data[base + 3];
                s4 += av * b_data[base + 4];
                s5 += av * b_data[base + 5];
                s6 += av * b_data[base + 6];
                s7 += av * b_data[base + 7];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
              out_row[j + 4] = s4;
              out_row[j + 5] = s5;
              out_row[j + 6] = s6;
              out_row[j + 7] = s7;
            }
          }
          if (n >= j4_threshold) {
            for (; j + 3 < n; j += 4) {
              double s0 = 0.0;
              double s1 = 0.0;
              double s2 = 0.0;
              double s3 = 0.0;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                const auto base = p * n + j;
                s0 += av * b_data[base + 0];
                s1 += av * b_data[base + 1];
                s2 += av * b_data[base + 2];
                s3 += av * b_data[base + 3];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
            }
          }
          for (; j < n; ++j) {
            double sum = 0.0;
            for (std::size_t p = 0; p < k; ++p) {
              sum += a_row[p] * b_data[p * n + j];
            }
            out_row[j] = sum;
          }
        }
      }
    }
  };
  run_parallel_row_tiles(
      m, n, k, tm, dispatch_cfg.default_thread_cap, dispatch_cfg.min_parallel_volume, run_tiles);
}

void run_own_gemm_f32_strict(const PackedMatrixView& a, const PackedMatrixView& b, bool b_transposed,
                             std::size_t m, std::size_t n, std::size_t k,
                             const MatmulSchedule& schedule, std::vector<float>& out) {
  out.assign(m * n, 0.0f);
  const auto tm = std::max<std::size_t>(8, schedule.tile_m);
  const auto* a_data = a.f32;
  const auto* b_data = b.f32;
  if (!a_data || !b_data) {
    throw EvalException("matmul_f32 strict kernel received invalid packed buffers");
  }
  auto* out_data = out.data();
  const auto dispatch_cfg = resolve_kernel_dispatch_config(true);
  const auto j8_threshold = dispatch_cfg.j8_threshold;
  const auto j4_threshold = dispatch_cfg.j4_threshold;
  const auto run_tiles = [&](std::size_t tile_begin, std::size_t tile_end) {
    for (std::size_t tile = tile_begin; tile < tile_end; ++tile) {
      const auto i0 = tile * tm;
      const auto i1 = std::min(i0 + tm, m);
      for (std::size_t i = i0; i < i1; ++i) {
        const auto* a_row = a_data + i * k;
        auto* out_row = out_data + i * n;
        std::size_t j = 0;
        if (b_transposed) {
          if (n >= j8_threshold) {
            for (; j + 7 < n; j += 8) {
              float s0 = 0.0f;
              float s1 = 0.0f;
              float s2 = 0.0f;
              float s3 = 0.0f;
              float s4 = 0.0f;
              float s5 = 0.0f;
              float s6 = 0.0f;
              float s7 = 0.0f;
              const auto* b0 = b_data + (j + 0) * k;
              const auto* b1 = b_data + (j + 1) * k;
              const auto* b2 = b_data + (j + 2) * k;
              const auto* b3 = b_data + (j + 3) * k;
              const auto* b4 = b_data + (j + 4) * k;
              const auto* b5 = b_data + (j + 5) * k;
              const auto* b6 = b_data + (j + 6) * k;
              const auto* b7 = b_data + (j + 7) * k;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                s0 += av * b0[p];
                s1 += av * b1[p];
                s2 += av * b2[p];
                s3 += av * b3[p];
                s4 += av * b4[p];
                s5 += av * b5[p];
                s6 += av * b6[p];
                s7 += av * b7[p];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
              out_row[j + 4] = s4;
              out_row[j + 5] = s5;
              out_row[j + 6] = s6;
              out_row[j + 7] = s7;
            }
          }
          if (n >= j4_threshold) {
            for (; j + 3 < n; j += 4) {
              float s0 = 0.0f;
              float s1 = 0.0f;
              float s2 = 0.0f;
              float s3 = 0.0f;
              const auto* b0 = b_data + (j + 0) * k;
              const auto* b1 = b_data + (j + 1) * k;
              const auto* b2 = b_data + (j + 2) * k;
              const auto* b3 = b_data + (j + 3) * k;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                s0 += av * b0[p];
                s1 += av * b1[p];
                s2 += av * b2[p];
                s3 += av * b3[p];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
            }
          }
          for (; j < n; ++j) {
            float sum = 0.0f;
            const auto* b_row = b_data + j * k;
            for (std::size_t p = 0; p < k; ++p) {
              sum += a_row[p] * b_row[p];
            }
            out_row[j] = sum;
          }
        } else {
          if (n >= j8_threshold) {
            for (; j + 7 < n; j += 8) {
              float s0 = 0.0f;
              float s1 = 0.0f;
              float s2 = 0.0f;
              float s3 = 0.0f;
              float s4 = 0.0f;
              float s5 = 0.0f;
              float s6 = 0.0f;
              float s7 = 0.0f;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                const auto base = p * n + j;
                s0 += av * b_data[base + 0];
                s1 += av * b_data[base + 1];
                s2 += av * b_data[base + 2];
                s3 += av * b_data[base + 3];
                s4 += av * b_data[base + 4];
                s5 += av * b_data[base + 5];
                s6 += av * b_data[base + 6];
                s7 += av * b_data[base + 7];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
              out_row[j + 4] = s4;
              out_row[j + 5] = s5;
              out_row[j + 6] = s6;
              out_row[j + 7] = s7;
            }
          }
          if (n >= j4_threshold) {
            for (; j + 3 < n; j += 4) {
              float s0 = 0.0f;
              float s1 = 0.0f;
              float s2 = 0.0f;
              float s3 = 0.0f;
              for (std::size_t p = 0; p < k; ++p) {
                const auto av = a_row[p];
                const auto base = p * n + j;
                s0 += av * b_data[base + 0];
                s1 += av * b_data[base + 1];
                s2 += av * b_data[base + 2];
                s3 += av * b_data[base + 3];
              }
              out_row[j + 0] = s0;
              out_row[j + 1] = s1;
              out_row[j + 2] = s2;
              out_row[j + 3] = s3;
            }
          }
          for (; j < n; ++j) {
            float sum = 0.0f;
            for (std::size_t p = 0; p < k; ++p) {
              sum += a_row[p] * b_data[p * n + j];
            }
            out_row[j] = sum;
          }
        }
      }
    }
  };
  run_parallel_row_tiles(
      m, n, k, tm, dispatch_cfg.default_thread_cap, dispatch_cfg.min_parallel_volume, run_tiles);
}

void run_own_gemm_f64(const PackedMatrixView& a, const PackedMatrixView& b, bool b_transposed,
                      std::size_t m, std::size_t n, std::size_t k,
                      const MatmulSchedule& schedule, std::vector<double>& out) {
  if (strict_fp_enabled()) {
    run_own_gemm_f64_strict(a, b, b_transposed, m, n, k, schedule, out);
    return;
  }
  out.assign(m * n, 0.0);
  const auto tm = std::max<std::size_t>(8, schedule.tile_m);
  const auto tn = std::max<std::size_t>(8, schedule.tile_n);
  const auto tk = std::max<std::size_t>(8, schedule.tile_k);
  const auto* a_data = a.f64;
  const auto* b_data = b.f64;
  if (!a_data || !b_data) {
    throw EvalException("matmul_f64 kernel received invalid packed buffers");
  }
  auto* out_data = out.data();
  const auto dispatch_cfg = resolve_kernel_dispatch_config(false);
  const auto j8_threshold = dispatch_cfg.j8_threshold;
  const auto j4_threshold = dispatch_cfg.j4_threshold;

  const auto run_tiles = [&](std::size_t tile_begin, std::size_t tile_end) {
    for (std::size_t tile = tile_begin; tile < tile_end; ++tile) {
      const auto i0 = tile * tm;
      const auto i1 = std::min(i0 + tm, m);
      for (std::size_t j0 = 0; j0 < n; j0 += tn) {
        const auto j1 = std::min(j0 + tn, n);
        for (std::size_t k0 = 0; k0 < k; k0 += tk) {
          const auto k1 = std::min(k0 + tk, k);
          const bool first_k_tile = (k0 == 0);
          if (b_transposed) {
            for (std::size_t i = i0; i < i1; ++i) {
              const auto* a_row = a_data + i * k;
              auto* out_row = out_data + i * n;
              std::size_t j = j0;
              if (n >= j8_threshold) {
                for (; j + 7 < j1; j += 8) {
                  auto s0 = first_k_tile ? 0.0 : out_row[j + 0];
                  auto s1 = first_k_tile ? 0.0 : out_row[j + 1];
                  auto s2 = first_k_tile ? 0.0 : out_row[j + 2];
                  auto s3 = first_k_tile ? 0.0 : out_row[j + 3];
                  auto s4 = first_k_tile ? 0.0 : out_row[j + 4];
                  auto s5 = first_k_tile ? 0.0 : out_row[j + 5];
                  auto s6 = first_k_tile ? 0.0 : out_row[j + 6];
                  auto s7 = first_k_tile ? 0.0 : out_row[j + 7];
                  const auto* b0 = b_data + (j + 0) * k;
                  const auto* b1 = b_data + (j + 1) * k;
                  const auto* b2 = b_data + (j + 2) * k;
                  const auto* b3 = b_data + (j + 3) * k;
                  const auto* b4 = b_data + (j + 4) * k;
                  const auto* b5 = b_data + (j + 5) * k;
                  const auto* b6 = b_data + (j + 6) * k;
                  const auto* b7 = b_data + (j + 7) * k;
                  std::size_t p = k0;
                  for (; p + 3 < k1; p += 4) {
                    const auto a0 = a_row[p + 0];
                    const auto a1 = a_row[p + 1];
                    const auto a2 = a_row[p + 2];
                    const auto a3 = a_row[p + 3];
                    s0 += a0 * b0[p + 0] + a1 * b0[p + 1] + a2 * b0[p + 2] + a3 * b0[p + 3];
                    s1 += a0 * b1[p + 0] + a1 * b1[p + 1] + a2 * b1[p + 2] + a3 * b1[p + 3];
                    s2 += a0 * b2[p + 0] + a1 * b2[p + 1] + a2 * b2[p + 2] + a3 * b2[p + 3];
                    s3 += a0 * b3[p + 0] + a1 * b3[p + 1] + a2 * b3[p + 2] + a3 * b3[p + 3];
                    s4 += a0 * b4[p + 0] + a1 * b4[p + 1] + a2 * b4[p + 2] + a3 * b4[p + 3];
                    s5 += a0 * b5[p + 0] + a1 * b5[p + 1] + a2 * b5[p + 2] + a3 * b5[p + 3];
                    s6 += a0 * b6[p + 0] + a1 * b6[p + 1] + a2 * b6[p + 2] + a3 * b6[p + 3];
                    s7 += a0 * b7[p + 0] + a1 * b7[p + 1] + a2 * b7[p + 2] + a3 * b7[p + 3];
                  }
                  for (; p < k1; ++p) {
                    const auto av = a_row[p];
                    s0 += av * b0[p];
                    s1 += av * b1[p];
                    s2 += av * b2[p];
                    s3 += av * b3[p];
                    s4 += av * b4[p];
                    s5 += av * b5[p];
                    s6 += av * b6[p];
                    s7 += av * b7[p];
                  }
                  out_row[j + 0] = s0;
                  out_row[j + 1] = s1;
                  out_row[j + 2] = s2;
                  out_row[j + 3] = s3;
                  out_row[j + 4] = s4;
                  out_row[j + 5] = s5;
                  out_row[j + 6] = s6;
                  out_row[j + 7] = s7;
                }
              }
              if (n >= j4_threshold) {
                for (; j + 3 < j1; j += 4) {
                  auto s0 = first_k_tile ? 0.0 : out_row[j + 0];
                  auto s1 = first_k_tile ? 0.0 : out_row[j + 1];
                  auto s2 = first_k_tile ? 0.0 : out_row[j + 2];
                  auto s3 = first_k_tile ? 0.0 : out_row[j + 3];
                  const auto* b0 = b_data + (j + 0) * k;
                  const auto* b1 = b_data + (j + 1) * k;
                  const auto* b2 = b_data + (j + 2) * k;
                  const auto* b3 = b_data + (j + 3) * k;
                  std::size_t p = k0;
                  for (; p + 3 < k1; p += 4) {
                    const auto a0 = a_row[p + 0];
                    const auto a1 = a_row[p + 1];
                    const auto a2 = a_row[p + 2];
                    const auto a3 = a_row[p + 3];
                    s0 += a0 * b0[p + 0] + a1 * b0[p + 1] + a2 * b0[p + 2] + a3 * b0[p + 3];
                    s1 += a0 * b1[p + 0] + a1 * b1[p + 1] + a2 * b1[p + 2] + a3 * b1[p + 3];
                    s2 += a0 * b2[p + 0] + a1 * b2[p + 1] + a2 * b2[p + 2] + a3 * b2[p + 3];
                    s3 += a0 * b3[p + 0] + a1 * b3[p + 1] + a2 * b3[p + 2] + a3 * b3[p + 3];
                  }
                  for (; p < k1; ++p) {
                    const auto av = a_row[p];
                    s0 += av * b0[p];
                    s1 += av * b1[p];
                    s2 += av * b2[p];
                    s3 += av * b3[p];
                  }
                  out_row[j + 0] = s0;
                  out_row[j + 1] = s1;
                  out_row[j + 2] = s2;
                  out_row[j + 3] = s3;
                }
              }
              for (; j < j1; ++j) {
                auto sum = first_k_tile ? 0.0 : out_row[j];
                const auto* b_row = b_data + j * k;
                std::size_t p = k0;
                for (; p + 3 < k1; p += 4) {
                  sum += a_row[p + 0] * b_row[p + 0] +
                         a_row[p + 1] * b_row[p + 1] +
                         a_row[p + 2] * b_row[p + 2] +
                         a_row[p + 3] * b_row[p + 3];
                }
                for (; p < k1; ++p) {
                  sum += a_row[p] * b_row[p];
                }
                out_row[j] = sum;
              }
            }
          } else {
            for (std::size_t i = i0; i < i1; ++i) {
              const auto* a_row = a_data + i * k;
              auto* out_row = out_data + i * n;
              for (std::size_t j = j0; j < j1; ++j) {
                auto sum = first_k_tile ? 0.0 : out_row[j];
                for (std::size_t p = k0; p < k1; ++p) {
                  sum += a_row[p] * b_data[p * n + j];
                }
                out_row[j] = sum;
              }
            }
          }
        }
      }
    }
  };

  run_parallel_row_tiles(
      m, n, k, tm, dispatch_cfg.default_thread_cap, dispatch_cfg.min_parallel_volume, run_tiles);
}

void run_own_gemm_f32(const PackedMatrixView& a, const PackedMatrixView& b, bool b_transposed,
                      std::size_t m, std::size_t n, std::size_t k,
                      const MatmulSchedule& schedule, std::vector<float>& out) {
  if (strict_fp_enabled()) {
    run_own_gemm_f32_strict(a, b, b_transposed, m, n, k, schedule, out);
    return;
  }
  out.assign(m * n, 0.0f);
  const auto tm = std::max<std::size_t>(8, schedule.tile_m);
  const auto tn = std::max<std::size_t>(8, schedule.tile_n);
  const auto tk = std::max<std::size_t>(8, schedule.tile_k);
  const auto* a_data = a.f32;
  const auto* b_data = b.f32;
  if (!a_data || !b_data) {
    throw EvalException("matmul_f32 kernel received invalid packed buffers");
  }
  auto* out_data = out.data();
  const auto dispatch_cfg = resolve_kernel_dispatch_config(true);
  const auto j8_threshold = dispatch_cfg.j8_threshold;
  const auto j4_threshold = dispatch_cfg.j4_threshold;

  const auto run_tiles = [&](std::size_t tile_begin, std::size_t tile_end) {
    for (std::size_t tile = tile_begin; tile < tile_end; ++tile) {
      const auto i0 = tile * tm;
      const auto i1 = std::min(i0 + tm, m);
      for (std::size_t j0 = 0; j0 < n; j0 += tn) {
        const auto j1 = std::min(j0 + tn, n);
        for (std::size_t k0 = 0; k0 < k; k0 += tk) {
          const auto k1 = std::min(k0 + tk, k);
          const bool first_k_tile = (k0 == 0);
          if (b_transposed) {
            for (std::size_t i = i0; i < i1; ++i) {
              const auto* a_row = a_data + i * k;
              auto* out_row = out_data + i * n;
              std::size_t j = j0;
              if (n >= j8_threshold) {
                for (; j + 7 < j1; j += 8) {
                  auto s0 = first_k_tile ? 0.0f : out_row[j + 0];
                  auto s1 = first_k_tile ? 0.0f : out_row[j + 1];
                  auto s2 = first_k_tile ? 0.0f : out_row[j + 2];
                  auto s3 = first_k_tile ? 0.0f : out_row[j + 3];
                  auto s4 = first_k_tile ? 0.0f : out_row[j + 4];
                  auto s5 = first_k_tile ? 0.0f : out_row[j + 5];
                  auto s6 = first_k_tile ? 0.0f : out_row[j + 6];
                  auto s7 = first_k_tile ? 0.0f : out_row[j + 7];
                  const auto* b0 = b_data + (j + 0) * k;
                  const auto* b1 = b_data + (j + 1) * k;
                  const auto* b2 = b_data + (j + 2) * k;
                  const auto* b3 = b_data + (j + 3) * k;
                  const auto* b4 = b_data + (j + 4) * k;
                  const auto* b5 = b_data + (j + 5) * k;
                  const auto* b6 = b_data + (j + 6) * k;
                  const auto* b7 = b_data + (j + 7) * k;
                  std::size_t p = k0;
                  for (; p + 3 < k1; p += 4) {
                    const auto a0 = a_row[p + 0];
                    const auto a1 = a_row[p + 1];
                    const auto a2 = a_row[p + 2];
                    const auto a3 = a_row[p + 3];
                    s0 += a0 * b0[p + 0] + a1 * b0[p + 1] + a2 * b0[p + 2] + a3 * b0[p + 3];
                    s1 += a0 * b1[p + 0] + a1 * b1[p + 1] + a2 * b1[p + 2] + a3 * b1[p + 3];
                    s2 += a0 * b2[p + 0] + a1 * b2[p + 1] + a2 * b2[p + 2] + a3 * b2[p + 3];
                    s3 += a0 * b3[p + 0] + a1 * b3[p + 1] + a2 * b3[p + 2] + a3 * b3[p + 3];
                    s4 += a0 * b4[p + 0] + a1 * b4[p + 1] + a2 * b4[p + 2] + a3 * b4[p + 3];
                    s5 += a0 * b5[p + 0] + a1 * b5[p + 1] + a2 * b5[p + 2] + a3 * b5[p + 3];
                    s6 += a0 * b6[p + 0] + a1 * b6[p + 1] + a2 * b6[p + 2] + a3 * b6[p + 3];
                    s7 += a0 * b7[p + 0] + a1 * b7[p + 1] + a2 * b7[p + 2] + a3 * b7[p + 3];
                  }
                  for (; p < k1; ++p) {
                    const auto av = a_row[p];
                    s0 += av * b0[p];
                    s1 += av * b1[p];
                    s2 += av * b2[p];
                    s3 += av * b3[p];
                    s4 += av * b4[p];
                    s5 += av * b5[p];
                    s6 += av * b6[p];
                    s7 += av * b7[p];
                  }
                  out_row[j + 0] = s0;
                  out_row[j + 1] = s1;
                  out_row[j + 2] = s2;
                  out_row[j + 3] = s3;
                  out_row[j + 4] = s4;
                  out_row[j + 5] = s5;
                  out_row[j + 6] = s6;
                  out_row[j + 7] = s7;
                }
              }
              if (n >= j4_threshold) {
                for (; j + 3 < j1; j += 4) {
                  auto s0 = first_k_tile ? 0.0f : out_row[j + 0];
                  auto s1 = first_k_tile ? 0.0f : out_row[j + 1];
                  auto s2 = first_k_tile ? 0.0f : out_row[j + 2];
                  auto s3 = first_k_tile ? 0.0f : out_row[j + 3];
                  const auto* b0 = b_data + (j + 0) * k;
                  const auto* b1 = b_data + (j + 1) * k;
                  const auto* b2 = b_data + (j + 2) * k;
                  const auto* b3 = b_data + (j + 3) * k;
                  std::size_t p = k0;
                  for (; p + 3 < k1; p += 4) {
                    const auto a0 = a_row[p + 0];
                    const auto a1 = a_row[p + 1];
                    const auto a2 = a_row[p + 2];
                    const auto a3 = a_row[p + 3];
                    s0 += a0 * b0[p + 0] + a1 * b0[p + 1] + a2 * b0[p + 2] + a3 * b0[p + 3];
                    s1 += a0 * b1[p + 0] + a1 * b1[p + 1] + a2 * b1[p + 2] + a3 * b1[p + 3];
                    s2 += a0 * b2[p + 0] + a1 * b2[p + 1] + a2 * b2[p + 2] + a3 * b2[p + 3];
                    s3 += a0 * b3[p + 0] + a1 * b3[p + 1] + a2 * b3[p + 2] + a3 * b3[p + 3];
                  }
                  for (; p < k1; ++p) {
                    const auto av = a_row[p];
                    s0 += av * b0[p];
                    s1 += av * b1[p];
                    s2 += av * b2[p];
                    s3 += av * b3[p];
                  }
                  out_row[j + 0] = s0;
                  out_row[j + 1] = s1;
                  out_row[j + 2] = s2;
                  out_row[j + 3] = s3;
                }
              }
              for (; j < j1; ++j) {
                auto sum = first_k_tile ? 0.0f : out_row[j];
                const auto* b_row = b_data + j * k;
                std::size_t p = k0;
                for (; p + 3 < k1; p += 4) {
                  sum += a_row[p + 0] * b_row[p + 0] +
                         a_row[p + 1] * b_row[p + 1] +
                         a_row[p + 2] * b_row[p + 2] +
                         a_row[p + 3] * b_row[p + 3];
                }
                for (; p < k1; ++p) {
                  sum += a_row[p] * b_row[p];
                }
                out_row[j] = sum;
              }
            }
          } else {
            for (std::size_t i = i0; i < i1; ++i) {
              const auto* a_row = a_data + i * k;
              auto* out_row = out_data + i * n;
              for (std::size_t j = j0; j < j1; ++j) {
                auto sum = first_k_tile ? 0.0f : out_row[j];
                for (std::size_t p = k0; p < k1; ++p) {
                  sum += a_row[p] * b_data[p * n + j];
                }
                out_row[j] = sum;
              }
            }
          }
        }
      }
    }
  };

  run_parallel_row_tiles(
      m, n, k, tm, dispatch_cfg.default_thread_cap, dispatch_cfg.min_parallel_volume, run_tiles);
}

}  // namespace

void run_matmul_f64_kernel(const Value& lhs, const Value& rhs, const MatmulSchedule& schedule,
                           std::vector<double>& out, MatmulBackend& backend_used) {
  const auto m = lhs.matrix_value->rows;
  const auto k = lhs.matrix_value->cols;
  const auto n = rhs.matrix_value->cols;

  bool a_cache_hit = false;
  const auto a = acquire_packed_matrix(lhs, false, false, a_cache_hit);
  record_pack_event(true, a_cache_hit);

  // BLAS is allowed in strict mode as long as strict-fp compile policy remains
  // intact (no fast-math). This enables high-performance backend requests
  // without enabling relaxed numerical flags.
  const bool use_blas = schedule.backend == MatmulBackend::Blas && has_blas_backend();
  const bool b_transposed = !use_blas && schedule.pack_b;

  bool b_cache_hit = false;
  const auto b = acquire_packed_matrix(rhs, b_transposed, false, b_cache_hit);
  record_pack_event(false, b_cache_hit);

  if (a.rows != m || a.cols != k) {
    throw EvalException("matmul() packed A shape mismatch");
  }
  if (b_transposed) {
    if (b.rows != n || b.cols != k) {
      throw EvalException("matmul() packed B^T shape mismatch");
    }
  } else if (b.rows != k || b.cols != n) {
    throw EvalException("matmul() packed B shape mismatch");
  }

  if (use_blas) {
    const auto& symbols = blas_symbols();
    if (!symbols.ready || !symbols.dgemm) {
      throw EvalException("matmul() BLAS backend requested but unavailable");
    }
    out.assign(m * n, 0.0);
    symbols.dgemm(kCblasRowMajor, kCblasNoTrans, kCblasNoTrans,
                  static_cast<int>(m), static_cast<int>(n), static_cast<int>(k),
                  1.0, a.f64, static_cast<int>(k),
                  b.f64, static_cast<int>(n),
                  0.0, out.data(), static_cast<int>(n));
    backend_used = MatmulBackend::Blas;
    record_backend_call(MatmulBackend::Blas);
    return;
  }

  run_own_gemm_f64(a, b, b_transposed, m, n, k, schedule, out);
  backend_used = MatmulBackend::Own;
  record_backend_call(MatmulBackend::Own);
}

void run_matmul_f32_kernel(const Value& lhs, const Value& rhs, const MatmulSchedule& schedule,
                           std::vector<float>& out, MatmulBackend& backend_used) {
  const auto m = lhs.matrix_value->rows;
  const auto k = lhs.matrix_value->cols;
  const auto n = rhs.matrix_value->cols;

  bool a_cache_hit = false;
  const auto a = acquire_packed_matrix(lhs, false, true, a_cache_hit);
  record_pack_event(true, a_cache_hit);

  const bool use_blas = schedule.backend == MatmulBackend::Blas && has_blas_backend();
  const bool b_transposed = !use_blas && schedule.pack_b;

  bool b_cache_hit = false;
  const auto b = acquire_packed_matrix(rhs, b_transposed, true, b_cache_hit);
  record_pack_event(false, b_cache_hit);

  if (a.rows != m || a.cols != k) {
    throw EvalException("matmul_f32() packed A shape mismatch");
  }
  if (b_transposed) {
    if (b.rows != n || b.cols != k) {
      throw EvalException("matmul_f32() packed B^T shape mismatch");
    }
  } else if (b.rows != k || b.cols != n) {
    throw EvalException("matmul_f32() packed B shape mismatch");
  }

  if (use_blas) {
    const auto& symbols = blas_symbols();
    if (!symbols.ready || !symbols.sgemm) {
      throw EvalException("matmul_f32() BLAS backend requested but unavailable");
    }
    out.assign(m * n, 0.0f);
    symbols.sgemm(kCblasRowMajor, kCblasNoTrans, kCblasNoTrans,
                  static_cast<int>(m), static_cast<int>(n), static_cast<int>(k),
                  1.0f, a.f32, static_cast<int>(k),
                  b.f32, static_cast<int>(n),
                  0.0f, out.data(), static_cast<int>(n));
    backend_used = MatmulBackend::Blas;
    record_backend_call(MatmulBackend::Blas);
    return;
  }

  run_own_gemm_f32(a, b, b_transposed, m, n, k, schedule, out);
  backend_used = MatmulBackend::Own;
  record_backend_call(MatmulBackend::Own);
}

}  // namespace matmul_schedule_gpu
}  // namespace spark
