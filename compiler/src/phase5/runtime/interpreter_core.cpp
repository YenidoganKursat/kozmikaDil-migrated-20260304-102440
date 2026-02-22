#include <memory>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <limits>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

namespace {

std::size_t utf8_codepoint_count(std::string_view text) {
  std::size_t count = 0;
  for (unsigned char ch : text) {
    if ((ch & 0xC0u) != 0x80u) {
      ++count;
    }
  }
  return count;
}

std::size_t utf8_to_utf16_units(std::string_view text) {
  std::size_t units = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto c0 = static_cast<unsigned char>(text[i]);
    std::uint32_t cp = 0xFFFDu;
    std::size_t advance = 1;
    if ((c0 & 0x80u) == 0u) {
      cp = c0;
      advance = 1;
    } else if ((c0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
      const auto c1 = static_cast<unsigned char>(text[i + 1]);
      cp = (static_cast<std::uint32_t>(c0 & 0x1Fu) << 6) |
           static_cast<std::uint32_t>(c1 & 0x3Fu);
      advance = 2;
    } else if ((c0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
      const auto c1 = static_cast<unsigned char>(text[i + 1]);
      const auto c2 = static_cast<unsigned char>(text[i + 2]);
      cp = (static_cast<std::uint32_t>(c0 & 0x0Fu) << 12) |
           (static_cast<std::uint32_t>(c1 & 0x3Fu) << 6) |
           static_cast<std::uint32_t>(c2 & 0x3Fu);
      advance = 3;
    } else if ((c0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
      const auto c1 = static_cast<unsigned char>(text[i + 1]);
      const auto c2 = static_cast<unsigned char>(text[i + 2]);
      const auto c3 = static_cast<unsigned char>(text[i + 3]);
      cp = (static_cast<std::uint32_t>(c0 & 0x07u) << 18) |
           (static_cast<std::uint32_t>(c1 & 0x3Fu) << 12) |
           (static_cast<std::uint32_t>(c2 & 0x3Fu) << 6) |
           static_cast<std::uint32_t>(c3 & 0x3Fu);
      advance = 4;
    }
    units += (cp > 0xFFFFu) ? 2u : 1u;
    i += advance;
  }
  return units;
}

}  // namespace

Interpreter::Interpreter() {
  reset();
}

void Interpreter::reset() {
  globals = std::make_shared<Environment>(nullptr);
  current_env = globals;

  auto print_fn = Value::builtin("print", [](const std::vector<Value>& args) -> Value {
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i > 0) {
        std::cout << " ";
      }
      std::cout << args[i].to_string();
    }
    std::cout << "\n";
    return Value::nil();
  });

  auto range_fn = Value::builtin("range", [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args.size() > 3) {
      throw EvalException("range() expects 1 to 3 integer arguments");
    }

    long long start = 0;
    long long stop = 0;
    long long step = 1;

    if (args.size() == 1) {
      stop = value_to_int(args[0]);
    } else {
      start = value_to_int(args[0]);
      stop = value_to_int(args[1]);
      if (args.size() == 3) {
        step = value_to_int(args[2]);
      }
    }

    if (step == 0) {
      throw EvalException("range() step must not be zero");
    }
    std::vector<Value> result;
    if (step > 0) {
      for (long long i = start; i < stop; i += step) {
        result.push_back(Value::int_value_of(i));
      }
    } else {
      for (long long i = start; i > stop; i += step) {
        result.push_back(Value::int_value_of(i));
      }
    }
    return Value::list_value_of(std::move(result));
  });

  auto len_fn = Value::builtin("len", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
      throw EvalException("len() expects exactly one argument");
    }
    if (args[0].kind == Value::Kind::List) {
      const auto& list = args[0];
      if (list.list_value.empty() &&
          list.list_cache.materialized_version == list.list_cache.version &&
          !list.list_cache.promoted_f64.empty()) {
        return Value::int_value_of(static_cast<long long>(list.list_cache.promoted_f64.size()));
      }
      return Value::int_value_of(static_cast<long long>(args[0].list_value.size()));
    }
    if (args[0].kind == Value::Kind::Matrix && args[0].matrix_value) {
      return Value::int_value_of(static_cast<long long>(args[0].matrix_value->rows));
    }
    if (args[0].kind == Value::Kind::String) {
      return Value::int_value_of(static_cast<long long>(utf8_codepoint_count(args[0].string_value)));
    }
    throw EvalException("len() currently supports list, matrix, or string values");
  });

  auto utf8_len_fn = Value::builtin("utf8_len", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1 || args[0].kind != Value::Kind::String) {
      throw EvalException("utf8_len() expects exactly one string argument");
    }
    return Value::int_value_of(static_cast<long long>(args[0].string_value.size()));
  });

  auto utf16_len_fn = Value::builtin("utf16_len", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1 || args[0].kind != Value::Kind::String) {
      throw EvalException("utf16_len() expects exactly one string argument");
    }
    return Value::int_value_of(static_cast<long long>(utf8_to_utf16_units(args[0].string_value)));
  });

  auto string_fn = Value::builtin("string", [](const std::vector<Value>& args) -> Value {
    if (args.size() > 1) {
      throw EvalException("string() expects zero or one argument");
    }
    if (args.empty()) {
      return Value::string_value_of("");
    }
    if (args[0].kind == Value::Kind::String) {
      return args[0];
    }
    return Value::string_value_of(args[0].to_string());
  });

  auto bench_tick_fn = Value::builtin("bench_tick", [](const std::vector<Value>& args) -> Value {
    if (!args.empty()) {
      throw EvalException("bench_tick() expects no arguments");
    }
#if defined(__APPLE__)
    static const mach_timebase_info_data_t timebase = [] {
      mach_timebase_info_data_t info{};
      mach_timebase_info(&info);
      if (info.denom == 0U) {
        info.numer = 1U;
        info.denom = 1U;
      }
      return info;
    }();
    static const bool one_to_one = (timebase.numer == 1U && timebase.denom == 1U);
    static const long double tick_to_ns =
        static_cast<long double>(timebase.numer) / static_cast<long double>(timebase.denom);
    const std::uint64_t ticks = mach_absolute_time();
    if (one_to_one) {
      return Value::int_value_of(static_cast<long long>(ticks));
    }
    const auto ns = static_cast<long long>(static_cast<long double>(ticks) * tick_to_ns);
    return Value::int_value_of(ns);
#elif defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    const auto ns =
        static_cast<long long>(ts.tv_sec) * 1000000000LL + static_cast<long long>(ts.tv_nsec);
    return Value::int_value_of(ns);
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return Value::int_value_of(static_cast<long long>(ns));
#endif
  });

  auto bench_tick_raw_fn = Value::builtin("bench_tick_raw", [](const std::vector<Value>& args) -> Value {
    if (!args.empty()) {
      throw EvalException("bench_tick_raw() expects no arguments");
    }
#if defined(__APPLE__)
    return Value::int_value_of(static_cast<long long>(mach_absolute_time()));
#elif defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    const auto ns =
        static_cast<long long>(ts.tv_sec) * 1000000000LL + static_cast<long long>(ts.tv_nsec);
    return Value::int_value_of(ns);
#else
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return Value::int_value_of(static_cast<long long>(ns));
#endif
  });

  auto bench_tick_scale_num_fn =
      Value::builtin("bench_tick_scale_num", [](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          throw EvalException("bench_tick_scale_num() expects no arguments");
        }
#if defined(__APPLE__)
        static const mach_timebase_info_data_t timebase = [] {
          mach_timebase_info_data_t info{};
          mach_timebase_info(&info);
          if (info.denom == 0U) {
            info.numer = 1U;
            info.denom = 1U;
          }
          return info;
        }();
        return Value::int_value_of(static_cast<long long>(timebase.numer));
#else
        return Value::int_value_of(1);
#endif
      });

  auto bench_tick_scale_den_fn =
      Value::builtin("bench_tick_scale_den", [](const std::vector<Value>& args) -> Value {
        if (!args.empty()) {
          throw EvalException("bench_tick_scale_den() expects no arguments");
        }
#if defined(__APPLE__)
        static const mach_timebase_info_data_t timebase = [] {
          mach_timebase_info_data_t info{};
          mach_timebase_info(&info);
          if (info.denom == 0U) {
            info.numer = 1U;
            info.denom = 1U;
          }
          return info;
        }();
        return Value::int_value_of(static_cast<long long>(timebase.denom));
#else
        return Value::int_value_of(1);
#endif
      });

  auto bench_mixed_numeric_op_fn =
      Value::builtin("bench_mixed_numeric_op", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3 || args.size() > 5) {
          throw EvalException(
              "bench_mixed_numeric_op() expects kind, operator, loops, optional seed_x, seed_y");
        }
        if (args[0].kind != Value::Kind::String || args[1].kind != Value::Kind::String) {
          throw EvalException("bench_mixed_numeric_op() kind/operator must be strings");
        }
        const auto loops = value_to_int(args[2]);
        long long seed_x = 123456789;
        long long seed_y = 362436069;
        if (args.size() >= 4) {
          seed_x = value_to_int(args[3]);
        }
        if (args.size() >= 5) {
          seed_y = value_to_int(args[4]);
        }
        return bench_mixed_numeric_op_runtime(args[0].string_value, args[1].string_value, loops, seed_x,
                                             seed_y);
      });

  auto cols_fn = Value::builtin("cols", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
      throw EvalException("cols() expects exactly one argument");
    }
    if (args[0].kind == Value::Kind::Matrix && args[0].matrix_value) {
      return Value::int_value_of(static_cast<long long>(args[0].matrix_value->cols));
    }
    throw EvalException("cols() currently supports only matrix values");
  });

  auto matrix_i64_fn = Value::builtin("matrix_i64", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("matrix_i64() expects exactly two integer arguments");
    }
    const auto rows_raw = value_to_int(args[0]);
    const auto cols_raw = value_to_int(args[1]);
    if (rows_raw < 0 || cols_raw < 0) {
      throw EvalException("matrix_i64() dimensions must be non-negative");
    }
    const auto rows = static_cast<std::size_t>(rows_raw);
    const auto cols = static_cast<std::size_t>(cols_raw);
    std::vector<Value> data;
    data.assign(rows * cols, Value::int_value_of(0));
    return Value::matrix_value_of(rows, cols, std::move(data));
  });

  auto matrix_f64_fn = Value::builtin("matrix_f64", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("matrix_f64() expects exactly two integer arguments");
    }
    const auto rows_raw = value_to_int(args[0]);
    const auto cols_raw = value_to_int(args[1]);
    if (rows_raw < 0 || cols_raw < 0) {
      throw EvalException("matrix_f64() dimensions must be non-negative");
    }
    const auto rows = static_cast<std::size_t>(rows_raw);
    const auto cols = static_cast<std::size_t>(cols_raw);
    std::vector<Value> data;
    data.assign(rows * cols, Value::double_value_of(0.0));
    return Value::matrix_value_of(rows, cols, std::move(data));
  });

  auto list_fill_affine_fn = Value::builtin("list_fill_affine", [](const std::vector<Value>& args) -> Value {
    if (args.size() < 5 || args.size() > 6) {
      throw EvalException("list_fill_affine() expects 5 or 6 arguments");
    }
    const auto n_raw = value_to_int(args[0]);
    const auto mul_i = value_to_int(args[1]);
    const auto add_i = value_to_int(args[2]);
    const auto mod = value_to_int(args[3]);
    if (n_raw < 0 || mod <= 0) {
      throw EvalException("list_fill_affine() invalid size or modulus");
    }
    if (args[4].kind != Value::Kind::Int && args[4].kind != Value::Kind::Double) {
      throw EvalException("list_fill_affine() scale must be numeric");
    }
    const auto scale = (args[4].kind == Value::Kind::Int) ? static_cast<double>(args[4].int_value)
                                                           : args[4].double_value;
    double bias = 0.0;
    if (args.size() == 6) {
      if (args[5].kind != Value::Kind::Int && args[5].kind != Value::Kind::Double) {
        throw EvalException("list_fill_affine() bias must be numeric");
      }
      bias = (args[5].kind == Value::Kind::Int) ? static_cast<double>(args[5].int_value)
                                                 : args[5].double_value;
    }

    const auto n = static_cast<std::size_t>(n_raw);
    const bool eager_dense_cache = n >= (128u * 1024u);

    // Dense-only mode is opt-in and only meaningful after cache threshold is reached.
    // env_flag_enabled() is the canonical parser to keep flag semantics consistent.
    const bool dense_only =
        env_flag_enabled("SPARK_LIST_FILL_DENSE_ONLY", false) && eager_dense_cache;

    std::vector<Value> data;
    if (!dense_only) {
      data.resize(n);
    }

    std::vector<double> dense_f64;
    if (eager_dense_cache) {
      dense_f64.resize(n);
    }

    const auto normalize_mod = [mod](long long value) {
      auto rem = value % mod;
      if (rem < 0) {
        rem += mod;
      }
      return rem;
    };
    const auto step = normalize_mod(mul_i);
    long long rem = normalize_mod(add_i);
    double reduce_sum = 0.0;

    for (std::size_t i = 0; i < n; ++i) {
      const auto value = static_cast<double>(rem) * scale + bias;
      if (!dense_only) {
        auto& cell = data[i];
        cell.kind = Value::Kind::Double;
        cell.double_value = value;
      }
      if (eager_dense_cache) {
        dense_f64[i] = value;
      }
      reduce_sum += value;
      rem += step;
      if (rem >= mod) {
        rem -= mod;
      }
    }

    auto result = Value::list_value_of(std::move(data));
    result.list_cache.live_plan = true;
    result.list_cache.plan = Value::LayoutTag::PackedDouble;
    result.list_cache.operation = "list_fill_affine";
    result.list_cache.analyzed_version = result.list_cache.version;
    if (eager_dense_cache) {
      result.list_cache.materialized_version = result.list_cache.version;
      result.list_cache.promoted_f64 = std::move(dense_f64);
      result.list_cache.reduced_sum_version = result.list_cache.version;
      result.list_cache.reduced_sum_value = reduce_sum;
      result.list_cache.reduced_sum_is_int = false;
    } else {
      result.list_cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
      result.list_cache.promoted_f64.clear();
      result.list_cache.reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
      result.list_cache.reduced_sum_value = 0.0;
      result.list_cache.reduced_sum_is_int = false;
    }
    return result;
  });

  auto matrix_fill_affine_fn = Value::builtin("matrix_fill_affine", [](const std::vector<Value>& args) -> Value {
    if (args.size() < 6 || args.size() > 7) {
      throw EvalException("matrix_fill_affine() expects 6 or 7 arguments");
    }
    const auto rows_raw = value_to_int(args[0]);
    const auto cols_raw = value_to_int(args[1]);
    const auto mul_i = value_to_int(args[2]);
    const auto mul_j = value_to_int(args[3]);
    const auto mod = value_to_int(args[4]);
    if (rows_raw < 0 || cols_raw < 0 || mod <= 0) {
      throw EvalException("matrix_fill_affine() invalid dimension or modulus");
    }
    if (args[5].kind != Value::Kind::Int && args[5].kind != Value::Kind::Double) {
      throw EvalException("matrix_fill_affine() scale must be numeric");
    }
    const auto scale = (args[5].kind == Value::Kind::Int) ? static_cast<double>(args[5].int_value)
                                                           : args[5].double_value;
    double bias = 0.0;
    if (args.size() == 7) {
      if (args[6].kind != Value::Kind::Int && args[6].kind != Value::Kind::Double) {
        throw EvalException("matrix_fill_affine() bias must be numeric");
      }
      bias = (args[6].kind == Value::Kind::Int) ? static_cast<double>(args[6].int_value)
                                                 : args[6].double_value;
    }

    const auto rows = static_cast<std::size_t>(rows_raw);
    const auto cols = static_cast<std::size_t>(cols_raw);
    const auto total = rows * cols;
    const bool eager_dense_cache = total >= (128u * 128u);

    // Same flag parser path as list_fill_affine to avoid diverging config behavior.
    const bool dense_only =
        env_flag_enabled("SPARK_MATRIX_FILL_DENSE_ONLY", false) && eager_dense_cache;

    std::vector<Value> data;
    if (!dense_only) {
      data.resize(total);
    }

    std::vector<double> dense_f64;
    if (eager_dense_cache) {
      dense_f64.resize(total);
    }

    const auto normalize_mod = [mod](long long value) {
      auto rem = value % mod;
      if (rem < 0) {
        rem += mod;
      }
      return rem;
    };
    const auto step_i = normalize_mod(mul_i);
    const auto step_j = normalize_mod(mul_j);
    long long row_rem = 0;
    std::size_t index = 0;
    for (std::size_t i = 0; i < rows; ++i) {
      long long rem = row_rem;
      for (std::size_t j = 0; j < cols; ++j) {
        const auto value = static_cast<double>(rem) * scale + bias;
        if (!dense_only) {
          auto& cell = data[index];
          cell.kind = Value::Kind::Double;
          cell.double_value = value;
        }
        if (eager_dense_cache) {
          dense_f64[index] = value;
        }
        ++index;
        rem += step_j;
        if (rem >= mod) {
          rem -= mod;
        }
      }
      row_rem += step_i;
      if (row_rem >= mod) {
        row_rem -= mod;
      }
    }

    auto result = Value::matrix_value_of(rows, cols, std::move(data));
    result.matrix_cache.plan = Value::LayoutTag::PackedDouble;
    result.matrix_cache.live_plan = true;
    result.matrix_cache.operation = "matrix_fill_affine";
    result.matrix_cache.analyzed_version = result.matrix_cache.version;
    if (eager_dense_cache) {
      result.matrix_cache.materialized_version = result.matrix_cache.version;
      result.matrix_cache.promoted_f64 = std::move(dense_f64);
    } else {
      result.matrix_cache.materialized_version = std::numeric_limits<std::uint64_t>::max();
      result.matrix_cache.promoted_f64.clear();
    }
    return result;
  });

  auto matmul_expected_sum_fn = Value::builtin("matmul_expected_sum", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("matmul_expected_sum() expects exactly two matrix arguments");
    }
    if (args[0].kind != Value::Kind::Matrix || !args[0].matrix_value ||
        args[1].kind != Value::Kind::Matrix || !args[1].matrix_value) {
      throw EvalException("matmul_expected_sum() expects matrix arguments");
    }

    const auto& a = *args[0].matrix_value;
    const auto& b = *args[1].matrix_value;
    if (a.cols != b.rows) {
      throw EvalException("matmul_expected_sum() shape mismatch: lhs.cols must equal rhs.rows");
    }

    const auto packed_f64_if_ready = [](const Value& matrix) -> const std::vector<double>* {
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
    };

    const auto as_double = [](const Value& cell) {
      if (cell.kind == Value::Kind::Int) {
        return static_cast<double>(cell.int_value);
      }
      if (cell.kind == Value::Kind::Double) {
        return cell.double_value;
      }
      throw EvalException("matmul_expected_sum() expects numeric matrix cells");
    };

    const auto* a_packed = packed_f64_if_ready(args[0]);
    const auto* b_packed = packed_f64_if_ready(args[1]);

    std::vector<double> col_sums(a.cols, 0.0);
    if (a_packed) {
      for (std::size_t i = 0; i < a.rows; ++i) {
        const auto base = i * a.cols;
        for (std::size_t k = 0; k < a.cols; ++k) {
          col_sums[k] += (*a_packed)[base + k];
        }
      }
    } else {
      for (std::size_t i = 0; i < a.rows; ++i) {
        for (std::size_t k = 0; k < a.cols; ++k) {
          col_sums[k] += as_double(a.data[i * a.cols + k]);
        }
      }
    }

    std::vector<double> row_sums(b.rows, 0.0);
    if (b_packed) {
      for (std::size_t k = 0; k < b.rows; ++k) {
        const auto base = k * b.cols;
        for (std::size_t j = 0; j < b.cols; ++j) {
          row_sums[k] += (*b_packed)[base + j];
        }
      }
    } else {
      for (std::size_t k = 0; k < b.rows; ++k) {
        for (std::size_t j = 0; j < b.cols; ++j) {
          row_sums[k] += as_double(b.data[k * b.cols + j]);
        }
      }
    }

    double expected = 0.0;
    for (std::size_t k = 0; k < a.cols; ++k) {
      expected += col_sums[k] * row_sums[k];
    }
    return Value::double_value_of(expected);
  });

  // Fused hot path: computes sum(A*B) directly without materializing matrix cells.
  auto matmul_sum_fn = Value::builtin("matmul_sum", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("matmul_sum() expects exactly two matrix arguments");
    }
    if (args[0].kind != Value::Kind::Matrix || !args[0].matrix_value ||
        args[1].kind != Value::Kind::Matrix || !args[1].matrix_value) {
      throw EvalException("matmul_sum() expects matrix arguments");
    }
    Value lhs = args[0];
    return matrix_matmul_sum_value(lhs, args[1]);
  });

  auto matmul_sum_f32_fn = Value::builtin("matmul_sum_f32", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("matmul_sum_f32() expects exactly two matrix arguments");
    }
    if (args[0].kind != Value::Kind::Matrix || !args[0].matrix_value ||
        args[1].kind != Value::Kind::Matrix || !args[1].matrix_value) {
      throw EvalException("matmul_sum_f32() expects matrix arguments");
    }
    Value lhs = args[0];
    return matrix_matmul_sum_f32_value(lhs, args[1]);
  });

  auto matmul4_sum_fn = Value::builtin("matmul4_sum", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 4) {
      throw EvalException("matmul4_sum() expects exactly four matrix arguments");
    }
    for (const auto& arg : args) {
      if (arg.kind != Value::Kind::Matrix || !arg.matrix_value) {
        throw EvalException("matmul4_sum() expects matrix arguments");
      }
    }
    Value a = args[0];
    return matrix_matmul4_sum_value(a, args[1], args[2], args[3]);
  });

  auto matmul4_sum_f32_fn = Value::builtin("matmul4_sum_f32", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 4) {
      throw EvalException("matmul4_sum_f32() expects exactly four matrix arguments");
    }
    for (const auto& arg : args) {
      if (arg.kind != Value::Kind::Matrix || !arg.matrix_value) {
        throw EvalException("matmul4_sum_f32() expects matrix arguments");
      }
    }
    Value a = args[0];
    return matrix_matmul4_sum_f32_value(a, args[1], args[2], args[3]);
  });

  auto accumulate_sum_fn = Value::builtin("accumulate_sum", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("accumulate_sum() expects exactly two arguments: total and list/matrix");
    }
    if (!is_numeric_kind(args[0])) {
      throw EvalException("accumulate_sum() first argument must be numeric");
    }
    double total = to_number_for_compare(args[0]);
    const auto& container = args[1];
    if (container.kind == Value::Kind::List) {
      for (const auto& item : container.list_value) {
        if (!is_numeric_kind(item)) {
          throw EvalException("accumulate_sum() list elements must be numeric");
        }
        total += to_number_for_compare(item);
      }
      return Value::double_value_of(total);
    }
    if (container.kind == Value::Kind::Matrix && container.matrix_value) {
      if (container.matrix_cache.plan == Value::LayoutTag::PackedDouble) {
        const auto total_values = container.matrix_value->rows * container.matrix_value->cols;
        const bool dense_ready =
            container.matrix_cache.materialized_version == container.matrix_cache.version &&
            container.matrix_cache.promoted_f64.size() == total_values;
        if (dense_ready) {
          for (const auto entry : container.matrix_cache.promoted_f64) {
            total += entry;
          }
        } else {
          for (const auto& cell : container.matrix_value->data) {
            total += cell.double_value;
          }
        }
        return Value::double_value_of(total);
      }
      if (container.matrix_cache.plan == Value::LayoutTag::PackedInt) {
        for (const auto& cell : container.matrix_value->data) {
          total += static_cast<double>(cell.int_value);
        }
        return Value::double_value_of(total);
      }
      for (const auto& cell : container.matrix_value->data) {
        if (!is_numeric_kind(cell)) {
          throw EvalException("accumulate_sum() matrix elements must be numeric");
        }
        total += matrix_element_to_double(cell);
      }
      return Value::double_value_of(total);
    }
    throw EvalException("accumulate_sum() second argument must be list or matrix");
  });

  auto spawn_fn = Value::builtin("spawn", [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw EvalException("spawn() expects at least one callable argument");
    }
    std::vector<Value> task_args;
    task_args.reserve(args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
      task_args.push_back(args[i]);
    }
    return spawn_task_value(args[0], task_args);
  });

  auto join_fn = Value::builtin("join", [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args.size() > 2) {
      throw EvalException("join() expects task and optional timeout_ms");
    }
    std::optional<long long> timeout = std::nullopt;
    if (args.size() == 2) {
      timeout = value_to_int(args[1]);
    }
    return await_task_value(args[0], timeout);
  });

  // `deadline(ms)` is a small alias for timeout arguments so surface syntax can
  // express deadline-style intent while runtime still consumes timeout_ms.
  auto deadline_fn = Value::builtin("deadline", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
      throw EvalException("deadline() expects exactly one timeout_ms argument");
    }
    const auto timeout = value_to_int(args[0]);
    if (timeout < 0) {
      throw EvalException("deadline() expects non-negative timeout_ms");
    }
    return Value::int_value_of(timeout);
  });

  auto cancel_fn = Value::builtin("cancel", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
      throw EvalException("cancel() expects exactly one task argument");
    }
    if (args[0].kind != Value::Kind::Task || !args[0].task_value || !args[0].task_value->cancelled) {
      throw EvalException("cancel() expects task argument");
    }
    args[0].task_value->cancelled->store(true, std::memory_order_relaxed);
    return Value::nil();
  });

  auto task_group_fn = Value::builtin("task_group", [](const std::vector<Value>& args) -> Value {
    if (args.size() > 1) {
      throw EvalException("task_group() expects zero or one timeout_ms argument");
    }
    std::optional<long long> timeout = std::nullopt;
    if (args.size() == 1) {
      timeout = value_to_int(args[0]);
    }
    return make_task_group_value(timeout);
  });

  auto parallel_for_fn = Value::builtin("parallel_for", [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) {
      throw EvalException("parallel_for() expects start, stop, fn [, extra...]");
    }
    std::vector<Value> extra;
    extra.reserve(args.size() > 3 ? args.size() - 3 : 0);
    for (std::size_t i = 3; i < args.size(); ++i) {
      extra.push_back(args[i]);
    }
    return parallel_for_value(args[0], args[1], args[2], extra);
  });

  auto par_map_fn = Value::builtin("par_map", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("par_map() expects list and fn arguments");
    }
    return par_map_value(args[0], args[1]);
  });

  auto par_reduce_fn = Value::builtin("par_reduce", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 3) {
      throw EvalException("par_reduce() expects list, init, fn arguments");
    }
    return par_reduce_value(args[0], args[1], args[2]);
  });

  auto scheduler_stats_fn = Value::builtin("scheduler_stats", [](const std::vector<Value>& args) -> Value {
    if (!args.empty()) {
      throw EvalException("scheduler_stats() expects no arguments");
    }
    return scheduler_stats_value();
  });

  auto channel_fn = Value::builtin("channel", [](const std::vector<Value>& args) -> Value {
    if (args.size() > 1) {
      throw EvalException("channel() expects optional capacity argument");
    }
    std::optional<long long> capacity = std::nullopt;
    if (!args.empty()) {
      capacity = value_to_int(args[0]);
    }
    return channel_make_value(capacity);
  });

  auto send_fn = Value::builtin("send", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 2) {
      throw EvalException("send() expects channel and value");
    }
    auto channel = args[0];
    return channel_send_value(channel, args[1]);
  });

  auto recv_fn = Value::builtin("recv", [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args.size() > 2) {
      throw EvalException("recv() expects channel and optional timeout_ms");
    }
    auto channel = args[0];
    std::optional<long long> timeout = std::nullopt;
    if (args.size() == 2) {
      timeout = value_to_int(args[1]);
    }
    return channel_recv_value(channel, timeout);
  });

  auto close_fn = Value::builtin("close", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
      throw EvalException("close() expects channel");
    }
    auto channel = args[0];
    return channel_close_value(channel);
  });

  auto stream_fn = Value::builtin("stream", [](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
      throw EvalException("stream() expects channel");
    }
    auto channel = args[0];
    return stream_value(channel);
  });

  auto anext_fn = Value::builtin("anext", [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args.size() > 2) {
      throw EvalException("anext() expects stream and optional timeout_ms");
    }
    auto stream = args[0];
    std::optional<long long> timeout = std::nullopt;
    if (args.size() == 2) {
      timeout = value_to_int(args[1]);
    }
    return stream_next_value(stream, timeout);
  });

  globals->define("print", print_fn);
  globals->define("range", range_fn);
  globals->define("len", len_fn);
  globals->define("utf8_len", utf8_len_fn);
  globals->define("utf16_len", utf16_len_fn);
  globals->define("string", string_fn);
  globals->define("bench_tick", bench_tick_fn);
  globals->define("bench_tick_raw", bench_tick_raw_fn);
  globals->define("bench_tick_scale_num", bench_tick_scale_num_fn);
  globals->define("bench_tick_scale_den", bench_tick_scale_den_fn);
  globals->define("bench_mixed_numeric_op", bench_mixed_numeric_op_fn);
  globals->define("cols", cols_fn);
  globals->define("matrix_i64", matrix_i64_fn);
  globals->define("matrix_f64", matrix_f64_fn);
  globals->define("list_fill_affine", list_fill_affine_fn);
  globals->define("matrix_fill_affine", matrix_fill_affine_fn);
  globals->define("matmul_expected_sum", matmul_expected_sum_fn);
  globals->define("matmul_sum", matmul_sum_fn);
  globals->define("matmul_sum_f32", matmul_sum_f32_fn);
  globals->define("matmul4_sum", matmul4_sum_fn);
  globals->define("matmul4_sum_f32", matmul4_sum_f32_fn);
  globals->define("accumulate_sum", accumulate_sum_fn);
  globals->define("spawn", spawn_fn);
  globals->define("join", join_fn);
  globals->define("deadline", deadline_fn);
  globals->define("cancel", cancel_fn);
  globals->define("task_group", task_group_fn);
  globals->define("parallel_for", parallel_for_fn);
  globals->define("par_map", par_map_fn);
  globals->define("par_reduce", par_reduce_fn);
  globals->define("scheduler_stats", scheduler_stats_fn);
  globals->define("channel", channel_fn);
  globals->define("send", send_fn);
  globals->define("recv", recv_fn);
  globals->define("close", close_fn);
  globals->define("stream", stream_fn);
  globals->define("anext", anext_fn);
  register_numeric_primitive_builtins(globals);
  prewarm_numeric_runtime();
  globals->define("None", Value::nil());
}

Value Interpreter::run(const Program& program) {
  current_env = globals;
  Value result = Value::nil();
  try {
    for (const auto& stmt : program.body) {
      result = execute(*stmt, current_env);
    }
  } catch (const ReturnSignal& signal) {
    return signal.value;
  }
  return result;
}

Value Interpreter::run_source(const std::string& source) {
  Parser parser(source);
  auto program = parser.parse_program();
  return run(*program);
}

bool Interpreter::has_global(std::string name) const {
  return globals && globals->contains(name);
}

Value Interpreter::global(std::string name) const {
  if (!globals) {
    throw EvalException("interpreter has no global environment");
  }
  return globals->get(name);
}

std::unordered_map<std::string, Value> Interpreter::snapshot_globals() const {
  std::unordered_map<std::string, Value> out;
  if (!globals) {
    return out;
  }
  for (const auto& name : globals->keys()) {
    out.emplace(name, globals->get(name));
  }
  return out;
}

}  // namespace spark
