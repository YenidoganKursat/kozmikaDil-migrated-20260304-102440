#pragma once

#include <functional>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "spark/ast.h"

namespace spark {

struct Environment;

struct Value {
  enum class Kind { Nil, Int, Double, Bool, List, Matrix, Function, Builtin };
  enum class LayoutTag {
    Unknown = 0,
    PackedInt = 1,
    PackedDouble = 2,
    PromotedPackedDouble = 3,
    ChunkedUnion = 4,
    GatherScatter = 5,
    BoxedAny = 6,
  };
  enum class ElementTag {
    Int = 0,
    Double = 1,
    Bool = 2,
    Other = 3,
  };

  struct ChunkRun {
    std::size_t offset = 0;
    std::size_t length = 0;
    ElementTag tag = ElementTag::Other;
  };

  struct ListCache {
    std::uint64_t version = 0;
    std::uint64_t analyzed_version = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t materialized_version = std::numeric_limits<std::uint64_t>::max();
    LayoutTag plan = LayoutTag::Unknown;
    bool live_plan = false;
    std::string operation;
    std::vector<double> promoted_f64;
    std::vector<double> gather_values_f64;
    std::vector<std::size_t> gather_indices;
    std::vector<ChunkRun> chunks;
    std::size_t analyze_count = 0;
    std::size_t materialize_count = 0;
    std::size_t cache_hit_count = 0;
    std::size_t invalidation_count = 0;
  };

  struct MatrixCache {
    std::uint64_t version = 0;
    std::uint64_t analyzed_version = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t materialized_version = std::numeric_limits<std::uint64_t>::max();
    LayoutTag plan = LayoutTag::Unknown;
    bool live_plan = false;
    std::string operation;
    std::vector<double> promoted_f64;
    std::size_t analyze_count = 0;
    std::size_t materialize_count = 0;
    std::size_t cache_hit_count = 0;
    std::size_t invalidation_count = 0;
  };

  Kind kind = Kind::Nil;
  long long int_value = 0;
  double double_value = 0.0;
  bool bool_value = false;
  std::vector<Value> list_value;
  ListCache list_cache;

  struct MatrixValue {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::vector<Value> data;
  };

  struct Function {
    std::vector<std::string> params;
    const Program* program = nullptr;  // not owned; program owns function body statements
    const StmtList* body = nullptr;
    std::shared_ptr<Environment> closure;
  };

  struct Builtin {
    std::string name;
    std::function<Value(const std::vector<Value>&)> impl;
  };

  std::shared_ptr<Function> function_value;
  std::shared_ptr<Builtin> builtin_value;
  std::shared_ptr<MatrixValue> matrix_value;
  MatrixCache matrix_cache;

  static Value nil();
  static Value int_value_of(long long v);
  static Value double_value_of(double v);
  static Value bool_value_of(bool v);
  static Value list_value_of(std::vector<Value> values);
  static Value matrix_value_of(std::size_t rows, std::size_t cols, std::vector<Value> values);
  static Value function(std::shared_ptr<Function> fn);
  static Value builtin(std::string name, std::function<Value(const std::vector<Value>&)> impl);

  std::string to_string() const;
  bool equals(const Value& other) const;
};

struct Environment {
  explicit Environment(std::shared_ptr<Environment> parent_env = nullptr);

  void define(std::string name, const Value& value);
  bool set(std::string name, const Value& value);
  bool contains(const std::string& name) const;
  Value get(const std::string& name) const;
  Value* get_ptr(const std::string& name);
  const Value* get_ptr(const std::string& name) const;
  std::vector<std::string> keys() const;

  std::shared_ptr<Environment> parent;
  std::unordered_map<std::string, Value> values;
};

struct EvalException : public std::runtime_error {
  explicit EvalException(std::string msg) : std::runtime_error(std::move(msg)) {}
};

class Interpreter {
 public:
  Interpreter();

 Value run(const Program& program);
  Value run_source(const std::string& source);
  void reset();

  bool has_global(std::string name) const;
  Value global(std::string name) const;
  std::unordered_map<std::string, Value> snapshot_globals() const;

  Value evaluate(const Expr& expr, const std::shared_ptr<Environment>& env);
  Value execute(const Stmt& stmt, const std::shared_ptr<Environment>& env);

  static bool truthy(const Value& value);
  static double to_number(const Value& value);

  Value eval_binary(BinaryOp op, const Value& left, const Value& right) const;
  Value eval_unary(UnaryOp op, const Value& operand) const;

  struct ReturnSignal {
    Value value;
  };

 private:
  std::shared_ptr<Environment> globals;
  std::shared_ptr<Environment> current_env;
};

}  // namespace spark
