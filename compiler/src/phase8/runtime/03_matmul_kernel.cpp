#include <algorithm>
#include <vector>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {
namespace phase8 {

namespace {

constexpr int kCblasRowMajor = 101;
constexpr int kCblasNoTrans = 111;

void run_own_gemm_f64(const PackedMatrixView& a, const PackedMatrixView& b, bool b_transposed,
                      std::size_t m, std::size_t n, std::size_t k,
                      const MatmulSchedule& schedule, std::vector<double>& out) {
  out.assign(m * n, 0.0);
  const auto tm = std::max<std::size_t>(8, schedule.tile_m);
  const auto tn = std::max<std::size_t>(8, schedule.tile_n);
  const auto tk = std::max<std::size_t>(8, schedule.tile_k);
  const auto* a_data = a.f64;
  const auto* b_data = b.f64;
  if (!a_data || !b_data) {
    throw EvalException("matmul_f64 kernel received invalid packed buffers");
  }

  for (std::size_t i0 = 0; i0 < m; i0 += tm) {
    const auto i1 = std::min(i0 + tm, m);
    for (std::size_t j0 = 0; j0 < n; j0 += tn) {
      const auto j1 = std::min(j0 + tn, n);
      for (std::size_t k0 = 0; k0 < k; k0 += tk) {
        const auto k1 = std::min(k0 + tk, k);
        for (std::size_t i = i0; i < i1; ++i) {
          const auto* a_row = a_data + i * k;
          for (std::size_t j = j0; j < j1; ++j) {
            auto sum = out[i * n + j];
            if (b_transposed) {
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
            } else {
              for (std::size_t p = k0; p < k1; ++p) {
                sum += a_row[p] * b_data[p * n + j];
              }
            }
            out[i * n + j] = sum;
          }
        }
      }
    }
  }
}

void run_own_gemm_f32(const PackedMatrixView& a, const PackedMatrixView& b, bool b_transposed,
                      std::size_t m, std::size_t n, std::size_t k,
                      const MatmulSchedule& schedule, std::vector<float>& out) {
  out.assign(m * n, 0.0f);
  const auto tm = std::max<std::size_t>(8, schedule.tile_m);
  const auto tn = std::max<std::size_t>(8, schedule.tile_n);
  const auto tk = std::max<std::size_t>(8, schedule.tile_k);
  const auto* a_data = a.f32;
  const auto* b_data = b.f32;
  if (!a_data || !b_data) {
    throw EvalException("matmul_f32 kernel received invalid packed buffers");
  }

  for (std::size_t i0 = 0; i0 < m; i0 += tm) {
    const auto i1 = std::min(i0 + tm, m);
    for (std::size_t j0 = 0; j0 < n; j0 += tn) {
      const auto j1 = std::min(j0 + tn, n);
      for (std::size_t k0 = 0; k0 < k; k0 += tk) {
        const auto k1 = std::min(k0 + tk, k);
        for (std::size_t i = i0; i < i1; ++i) {
          const auto* a_row = a_data + i * k;
          for (std::size_t j = j0; j < j1; ++j) {
            auto sum = out[i * n + j];
            if (b_transposed) {
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
            } else {
              for (std::size_t p = k0; p < k1; ++p) {
                sum += a_row[p] * b_data[p * n + j];
              }
            }
            out[i * n + j] = sum;
          }
        }
      }
    }
  }
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

}  // namespace phase8
}  // namespace spark
