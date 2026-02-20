#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "spark/ast.h"

namespace spark {

struct Type {
  enum class Kind {
    Unknown,
    Nil,
    Int,
    Float,
    String,
    Bool,
    Any,
    List,
    Matrix,
    Task,
    TaskGroup,
    Channel,
    Function,
    Class,
    Builtin,
    Error,
  };

  enum class FloatKind {
    F8,
    F16,
    BF16,
    F32,
    F64,
    F128,
    F256,
    F512,
  };

  Kind kind = Kind::Unknown;
  std::vector<std::shared_ptr<Type>> function_params;
  std::shared_ptr<Type> function_return;
  std::shared_ptr<Type> list_element;
  std::size_t arity_min = 0;
  std::size_t arity_max = 0;

  FloatKind float_kind = FloatKind::F64;
  bool class_open = false;
  std::string class_name;
  std::size_t class_shape_id = 0;
  std::size_t matrix_rows = 0;
  std::size_t matrix_cols = 0;
  std::shared_ptr<Type> task_result;
  std::shared_ptr<Type> channel_element;
  std::size_t channel_capacity = 0;
};

using TypePtr = std::shared_ptr<Type>;

struct TypeError : public std::runtime_error {
  explicit TypeError(std::string msg) : std::runtime_error(std::move(msg)) {}
};

enum class TierLevel {
  T4,
  T5,
  T8,
};

struct TierReason {
  std::string message;
  bool normalizable = false;
};

struct SymbolRecord {
  std::string name;
  std::string owner;
  std::string scope;
  std::string type;
};

struct ShapeRecord {
  std::string name;
  std::string shape_id;
  bool open = false;
  std::vector<std::string> fields;
  std::vector<std::string> reasons;
};

struct TierRecord {
  std::string name;
  std::string parent;
  TierLevel tier = TierLevel::T8;
  std::string kind;
  std::vector<std::string> reasons;
};

struct PipelineRecord {
  std::string id;
  std::string receiver;
  std::string receiver_type;
  std::vector<std::string> nodes;
  std::string terminal;
  bool fused = false;
  bool materialize_required = false;
  std::vector<std::string> reasons;
};

struct AsyncLoweringRecord {
  std::string function_name;
  std::size_t await_points = 0;
  std::size_t states = 1;
  bool heap_frame = false;
};

class TypeChecker {
 public:
  explicit TypeChecker();

  void check(const Program& program);

  const std::vector<std::string>& diagnostics() const;
  bool has_errors() const;
  bool has_fatal_errors() const;

  std::string dump_types() const;
  std::string dump_shapes() const;
  std::string dump_tier_report() const;
  std::string dump_pipeline_ir() const;
  std::string dump_fusion_plan() const;
  std::string dump_why_not_fused() const;
  std::string dump_async_lowering() const;

  const std::vector<SymbolRecord>& symbols() const;
  const std::vector<ShapeRecord>& shapes() const;
  const std::vector<TierRecord>& function_reports() const;
  const std::vector<TierRecord>& loop_reports() const;
  const std::vector<PipelineRecord>& pipelines() const;
  const std::vector<AsyncLoweringRecord>& async_lowerings() const;

  static std::string type_to_string(const Type& type);
  static std::string tier_to_string(TierLevel tier);

 private:
  TypePtr bool_type();
  TypePtr int_type();
  TypePtr float_type(Type::FloatKind kind = Type::FloatKind::F64);
  TypePtr string_type();
  TypePtr nil_type();
  TypePtr unknown_type();
  TypePtr any_type() const;
  TypePtr error_type() const;
  TypePtr list_type(TypePtr element_type);
  TypePtr matrix_type(TypePtr element_type, std::size_t rows, std::size_t cols);
  TypePtr task_type(TypePtr result_type);
  TypePtr task_group_type();
  TypePtr channel_type(TypePtr element_type, std::size_t capacity = 0);
  TypePtr function_type(std::vector<TypePtr> params, TypePtr return_type);
  TypePtr builtin_type(std::vector<TypePtr> params, TypePtr return_type,
                       std::optional<std::size_t> min_arity = std::nullopt);
  TypePtr class_type(std::string_view name, bool open, std::size_t shape_id);

  void push_scope(std::string_view name = "global");
  void pop_scope();
  bool define_name(const std::string& name, TypePtr type);
  bool set_name(const std::string& name, TypePtr type);
  void register_symbol(const std::string& name, const std::string& owner,
                       const std::string& scope, const Type& type);
  bool has_name(const std::string& name) const;
  TypePtr get_name(const std::string& name) const;

  void add_error(const std::string& message);
  bool is_numeric_type(const Type& type) const;
  bool is_bool_like(const Type& type) const;
  bool same_or_unknown(const Type& a, const Type& b) const;
  bool is_assignable(const Type& source, const Type& target) const;
  TypePtr normalize_list_elements(const TypePtr& current, const TypePtr& next);

  void push_function_context(const std::string& name);
  void pop_function_context();
  void push_loop_context(const std::string& owner, const std::string& kind);
  void pop_loop_context();
  void add_context_reason(const TierReason& reason);
  TierLevel fold_tier(const std::vector<TierReason>& reasons) const;

  bool check_class_layout(const ClassDefStmt& cls, TypePtr class_type);

  void check_program_body(const StmtList& body);
  void check_function_body(const FunctionDefStmt& fn, TypePtr function_type);
  void check_class_body(const ClassDefStmt& cls, TypePtr class_ty);
  TypePtr check_call(const CallExpr& call);
  TypePtr infer_index_access_type(const IndexExpr& index, bool in_assignment);
  TypePtr infer_lvalue_type(const Expr& target);
  void check_unary(const UnaryExpr& unary);
  void check_binary(const BinaryExpr& binary);

  void analyze_expr(const Expr& expr, const TypePtr& type);
  TypePtr infer_expr(const Expr& expr);
  void check_stmt(const Stmt& stmt);

  std::vector<std::string> errors_;
  std::vector<std::unordered_map<std::string, TypePtr>> scopes_;
  std::vector<std::string> scope_names_;

  std::vector<SymbolRecord> symbols_;
  std::unordered_map<std::string, std::size_t> symbol_key_to_index_;

  std::vector<ShapeRecord> shapes_;
  std::vector<TierRecord> function_reports_;
  std::vector<TierRecord> loop_reports_;
  std::vector<PipelineRecord> pipelines_;
  std::vector<AsyncLoweringRecord> async_lowerings_;

  struct FunctionContext {
    std::string name;
    std::vector<TierReason> reasons;
  };

  struct LoopContext {
    std::string id;
    std::vector<TierReason> reasons;
  };

  std::vector<FunctionContext> function_stack_;
  std::vector<LoopContext> loop_stack_;
  std::vector<TierReason> main_context_reasons_;

  std::size_t next_shape_id_ = 0;
  bool suppress_side_effects_ = false;
};

}  // namespace spark
