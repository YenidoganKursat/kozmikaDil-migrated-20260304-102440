#include <memory>
#include <iostream>
#include <unordered_map>

#include "../../phase3/evaluator_parts/internal_helpers.h"

namespace spark {

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
      return Value::int_value_of(static_cast<long long>(args[0].list_value.size()));
    }
    if (args[0].kind == Value::Kind::Matrix && args[0].matrix_value) {
      return Value::int_value_of(static_cast<long long>(args[0].matrix_value->rows));
    }
    throw EvalException("len() currently supports only list or matrix values");
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
    std::vector<Value> data(rows * cols);
    for (std::size_t i = 0; i < rows; ++i) {
      for (std::size_t j = 0; j < cols; ++j) {
        auto raw = static_cast<long long>(i) * mul_i + static_cast<long long>(j) * mul_j;
        auto rem = raw % mod;
        if (rem < 0) {
          rem += mod;
        }
        const auto value = static_cast<double>(rem) * scale + bias;
        auto& cell = data[i * cols + j];
        cell.kind = Value::Kind::Double;
        cell.double_value = value;
      }
    }
    auto result = Value::matrix_value_of(rows, cols, std::move(data));
    result.matrix_cache.plan = Value::LayoutTag::PackedDouble;
    result.matrix_cache.live_plan = true;
    result.matrix_cache.operation = "matrix_fill_affine";
    result.matrix_cache.analyzed_version = result.matrix_cache.version;
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

    const auto as_double = [](const Value& cell) {
      if (cell.kind == Value::Kind::Int) {
        return static_cast<double>(cell.int_value);
      }
      if (cell.kind == Value::Kind::Double) {
        return cell.double_value;
      }
      throw EvalException("matmul_expected_sum() expects numeric matrix cells");
    };

    std::vector<double> col_sums(a.cols, 0.0);
    for (std::size_t i = 0; i < a.rows; ++i) {
      for (std::size_t k = 0; k < a.cols; ++k) {
        col_sums[k] += as_double(a.data[i * a.cols + k]);
      }
    }

    std::vector<double> row_sums(b.rows, 0.0);
    for (std::size_t k = 0; k < b.rows; ++k) {
      for (std::size_t j = 0; j < b.cols; ++j) {
        row_sums[k] += as_double(b.data[k * b.cols + j]);
      }
    }

    double expected = 0.0;
    for (std::size_t k = 0; k < a.cols; ++k) {
      expected += col_sums[k] * row_sums[k];
    }
    return Value::double_value_of(expected);
  });

  globals->define("print", print_fn);
  globals->define("range", range_fn);
  globals->define("len", len_fn);
  globals->define("cols", cols_fn);
  globals->define("matrix_i64", matrix_i64_fn);
  globals->define("matrix_f64", matrix_f64_fn);
  globals->define("matrix_fill_affine", matrix_fill_affine_fn);
  globals->define("matmul_expected_sum", matmul_expected_sum_fn);
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
