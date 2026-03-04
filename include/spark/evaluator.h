#pragma once

#include <functional>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "spark/ast.h"

namespace spark {

struct Environment;
struct TaskHandle;
struct TaskGroupHandle;
struct ChannelHandle;

struct Value {
  enum class Kind {
    Nil,
    Int,
    Double,
    String,
    Numeric,
    Bool,
    List,
    Matrix,
    Class,
    Object,
    Function,
    Builtin,
    Task,
    TaskGroup,
    Channel
  };
  enum class NumericKind {
    I8,
    I16,
    I32,
    I64,
    I128,
    I256,
    I512,
    F8,
    F16,
    BF16,
    F32,
    F64,
    F128,
    F256,
    F512,
  };
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
    std::uint64_t reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    double reduced_sum_value = 0.0;
    bool reduced_sum_is_int = false;
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
    std::uint64_t reduced_sum_version = std::numeric_limits<std::uint64_t>::max();
    double reduced_sum_value = 0.0;
    bool reduced_sum_is_int = false;
    std::size_t analyze_count = 0;
    std::size_t materialize_count = 0;
    std::size_t cache_hit_count = 0;
    std::size_t invalidation_count = 0;
  };

  Kind kind = Kind::Nil;
  long long int_value = 0;
  double double_value = 0.0;
  std::string string_value;
  bool bool_value = false;
  std::vector<Value> list_value;
  ListCache list_cache;

  struct MatrixValue {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::vector<Value> data;
  };

  struct ClassValue {
    std::string name;
    bool open_shape = false;
    std::unordered_map<std::string, Value> field_defaults;
    std::unordered_map<std::string, Value> methods;
  };

  struct ObjectValue {
    std::shared_ptr<ClassValue> class_def;
    std::unordered_map<std::string, Value> fields;
  };

  struct Function {
    std::vector<std::string> params;
    const Program* program = nullptr;  // not owned; program owns function body statements
    const StmtList* body = nullptr;
    bool is_async = false;
    std::shared_ptr<Environment> closure;
    std::shared_ptr<Environment> closure_frozen;
    std::unordered_map<std::string, Value> closure_snapshot;
  };

  enum class BuiltinTag : std::uint8_t {
    Generic = 0,
    BenchTick,
    BenchTickRaw,
    BenchTickScaleNum,
    BenchTickScaleDen,
    String,
    Len,
    Utf8Len,
    Utf16Len,
    NumericConstructor,
  };

  struct Builtin {
    std::string name;
    std::function<Value(const std::vector<Value>&)> impl;
    BuiltinTag tag = BuiltinTag::Generic;
    bool numeric_constructor = false;
    NumericKind numeric_constructor_kind = NumericKind::F64;
  };

  struct NumericValue {
    NumericKind kind = NumericKind::F64;
    std::uint64_t revision = 1;
    std::string payload;
    bool parsed_int_valid = false;
    __int128_t parsed_int = 0;
    bool parsed_float_valid = false;
    long double parsed_float = 0.0L;
    // Opaque runtime cache for high-precision numeric backends (e.g. MPFR).
    // Stored as void to keep public headers backend-agnostic.
    mutable std::shared_ptr<void> high_precision_cache;
  };

  std::shared_ptr<Function> function_value;
  std::shared_ptr<Builtin> builtin_value;
  std::optional<NumericValue> numeric_value;
  std::shared_ptr<MatrixValue> matrix_value;
  std::shared_ptr<ClassValue> class_value;
  std::shared_ptr<ObjectValue> object_value;
  std::shared_ptr<TaskHandle> task_value;
  std::shared_ptr<TaskGroupHandle> task_group_value;
  std::shared_ptr<ChannelHandle> channel_value;
  MatrixCache matrix_cache;

  static Value nil();
  static Value int_value_of(long long v);
  static Value double_value_of(double v);
  static Value string_value_of(std::string v);
  static Value numeric_value_of(NumericKind kind, std::string payload);
  static Value numeric_int_value_of(NumericKind kind, __int128_t v);
  static Value numeric_float_value_of(NumericKind kind, long double v);
  static Value bool_value_of(bool v);
  static Value list_value_of(std::vector<Value> values);
  static Value matrix_value_of(std::size_t rows, std::size_t cols, std::vector<Value> values);
  static Value class_value_of(std::shared_ptr<ClassValue> cls);
  static Value object_value_of(std::shared_ptr<ObjectValue> obj);
  static Value function(std::shared_ptr<Function> fn);
  static Value builtin(std::string name, std::function<Value(const std::vector<Value>&)> impl);
  static Value task_value_of(std::shared_ptr<TaskHandle> task);
  static Value task_group_value_of(std::shared_ptr<TaskGroupHandle> task_group);
  static Value channel_value_of(std::shared_ptr<ChannelHandle> channel);

  std::string to_string() const;
  bool equals(const Value& other) const;
};

struct Environment {
  explicit Environment(std::shared_ptr<Environment> parent_env = nullptr, bool is_frozen = false);

  void define(std::string name, const Value& value);
  void define(std::string name, Value&& value);
  bool set(std::string name, const Value& value);
  bool set(std::string name, Value&& value);
  bool contains(const std::string& name) const;
  Value get(const std::string& name) const;
  Value* get_ptr(const std::string& name);
  const Value* get_ptr(const std::string& name) const;
  std::vector<std::string> keys() const;

  std::shared_ptr<Environment> parent;
  bool frozen = false;
  std::uint64_t stable_id = 0;
  std::uint64_t values_epoch = 1;
  std::unordered_map<std::string, Value> values;
  // Positive owner-cache for lexical name resolution. This reduces repeated
  // parent-chain walks in hot loops while preserving strict semantics.
  mutable std::unordered_map<std::string, const Environment*> lookup_owner_cache;
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

  struct BreakSignal {};
  struct ContinueSignal {};

 private:
  std::shared_ptr<Environment> globals;
  std::shared_ptr<Environment> current_env;
};

struct TaskHandle {
  std::shared_future<Value> future;
  std::shared_ptr<std::atomic<bool>> cancelled;
  std::size_t scheduler_queue_hint = 0;
  bool has_scheduler_queue_hint = false;
};

struct TaskGroupHandle {
  std::mutex mutex;
  std::vector<std::shared_ptr<TaskHandle>> tasks;
  std::shared_ptr<std::atomic<bool>> cancelled;
  long long timeout_ms = -1;
};

struct ChannelHandle {
  mutable std::mutex mutex;
  std::condition_variable cv_send;
  std::condition_variable cv_recv;
  std::deque<std::shared_ptr<Value>> queue;
  std::size_t capacity = 0;  // 0 means unbounded.
  bool closed = false;
  std::size_t send_count = 0;
  std::size_t recv_count = 0;
  std::size_t wait_count = 0;
};

}  // namespace spark
