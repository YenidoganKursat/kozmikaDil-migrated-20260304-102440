#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "spark/ast.h"

namespace spark {

enum class ValueKind {
  Unknown,
  Int,
  Float,
  Bool,
  String,
  Void,
  Invalid,
  ListInt,
  ListFloat,
  ListAny,
  MatrixInt,
  MatrixFloat,
  MatrixAny,
};

using ScalarKind = ValueKind;

struct CodegenOptions {
  bool verbose = false;
};

struct CodegenResult {
  bool success = false;
  std::string output;
  std::vector<std::string> diagnostics;
};

struct IRToCOptions {
  bool emit_entry = true;
};

struct IRToCResult {
  bool success = false;
  std::string output;
  std::vector<std::string> diagnostics;
};

class CodeGenerator {
 public:
  struct Code {
    std::string value;
    ValueKind kind = ValueKind::Unknown;
    bool has_value = true;
    std::string numeric_hint;
  };

  CodegenResult generate(const Program& program, const CodegenOptions& options = {});

 private:
  enum class ExpectedExprContext {
    None,
    Int,
    Float,
    Bool,
    Void,
  };

  struct FunctionSignature {
    std::string name;
    std::vector<std::string> params;
    std::vector<ValueKind> param_kinds;
    ValueKind return_kind = ValueKind::Unknown;
  };

  struct FunctionContext {
    std::string name;
    bool is_main = false;
    bool has_terminated = false;
    ValueKind return_kind = ValueKind::Void;
    std::vector<ValueKind> return_types;
    std::vector<std::unordered_map<std::string, ValueKind>> scopes;
    std::unordered_map<std::string, ValueKind> container_element_kinds;
    std::unordered_map<std::string, std::string> scalar_numeric_hints;
    std::unordered_set<std::string> unchecked_append_targets;
  };

  std::string scalar_kind_to_name(ValueKind kind) const;
  std::string value_kind_to_name(ValueKind kind) const;
  std::string scalar_to_constant(double value, ScalarKind expected) const;
  std::string next_temp();
  std::string next_label();
  std::string indent() const;

  void emit_line(const std::string& line);
  void emit_label(const std::string& label);
  void begin_indent();
  void end_indent();

  bool infer_top_level(const Program& program);
  bool compile_top_level(const Program& program);
  bool compile_block(const std::vector<const Stmt*>& block, FunctionContext& ctx);
  using StmtCodegenHandler = bool (CodeGenerator::*)(const Stmt&, FunctionContext&);
  bool compile_stmt(const Stmt& stmt, FunctionContext& ctx);
  bool dispatch_stmt(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_expression(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_assign(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_return(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_break(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_continue(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_if(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_switch(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_try_catch(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_while(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_for(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_function_def(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_class_def(const Stmt& stmt, FunctionContext& ctx);
  bool compile_stmt_with_task_group(const Stmt& stmt, FunctionContext& ctx);

  bool emit_function(const FunctionDefStmt& fn);
  bool emit_main_body(const std::vector<const Stmt*>& body);
  Code emit_function_call(const CallExpr& call, FunctionContext& ctx,
                         ExpectedExprContext expected = ExpectedExprContext::None);
  Code emit_list_expression(const ListExpr& list, FunctionContext& ctx, ExpectedExprContext expected);
  Code emit_index_expression(const IndexExpr& index, FunctionContext& ctx, ExpectedExprContext expected);
  bool emit_for_matrix_statement(const ForStmt& for_stmt, const Code& iterable, FunctionContext& ctx);
  bool emit_indexed_assignment(const IndexExpr& target, const Code& value, FunctionContext& ctx);
  bool emit_if_statement(const IfStmt& if_stmt, FunctionContext& ctx);
  bool emit_while_statement(const WhileStmt& while_stmt, FunctionContext& ctx);
  bool emit_for_statement(const ForStmt& for_stmt, FunctionContext& ctx);
  bool emit_for_list_statement(const ForStmt& for_stmt, const Code& iterable, FunctionContext& ctx);
  bool emit_range_loop_setup(const Expr& iterable, FunctionContext& ctx, Code& start_code, Code& stop_code,
                            long long& step_value);

  Code emit_expr(const Expr& expr, FunctionContext& ctx,
                 ExpectedExprContext expected = ExpectedExprContext::None);

  ValueKind infer_list_expression_kind(const ListExpr& list) const;
  ValueKind infer_matrix_row_element_kind(const std::vector<std::string>& row_values, const std::vector<ValueKind>& kinds) const;

  ValueKind ensure_expected(ValueKind actual, ExpectedExprContext expected, const std::string& node);
  ValueKind ensure_bool_for_condition(Code& code, FunctionContext& ctx);
  ValueKind coerce_numeric(ValueKind left, ValueKind right) const;
  ValueKind merge_types(ValueKind left, ValueKind right) const;
  ValueKind finalize_return_type(const std::vector<ValueKind>& types) const;

  ValueKind lookup_var_type(const FunctionContext& ctx, const std::string& name) const;
  bool has_var(const FunctionContext& ctx, const std::string& name) const;
  void set_var_type(FunctionContext& ctx, const std::string& name, ValueKind kind);
  std::string lookup_numeric_hint(const FunctionContext& ctx, const std::string& name) const;
  void set_numeric_hint(FunctionContext& ctx, const std::string& name, const std::string& hint);
  void clear_numeric_hint(FunctionContext& ctx, const std::string& name);
  ValueKind lookup_container_element_type(const FunctionContext& ctx, const std::string& name) const;
  void set_container_element_type(FunctionContext& ctx, const std::string& name, ValueKind kind);
  ValueKind infer_container_scalar_type(ValueKind container_kind) const;
  ValueKind default_container_element_for(const std::string& variable_name, const std::string& container_name) const;
  void emit_var_decl_if_needed(FunctionContext& ctx, const std::string& name, ValueKind kind);

  void push_scope(FunctionContext& ctx);
  void pop_scope(FunctionContext& ctx);
  void merge_scopes(FunctionContext& base, const FunctionContext& branch);

  void emit_default_return(const FunctionContext& ctx);

  void add_error(const std::string& message);
  const FunctionSignature* find_function_signature(const std::string& name) const;
  FunctionSignature* find_function_signature_mut(const std::string& name);

  std::vector<std::string> diagnostics_;
  std::ostringstream output_;
  std::size_t temp_id_ = 0;
  std::size_t label_id_ = 0;
  std::size_t indent_level_ = 0;
  std::vector<FunctionSignature> functions_;
  bool verbose_ = false;
};

class IRToCGenerator {
 public:
  IRToCResult translate(const std::string& ir, const IRToCOptions& options = {});
};

}  // namespace spark
