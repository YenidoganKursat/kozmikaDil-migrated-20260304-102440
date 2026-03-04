#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spark {

struct Expr;
struct Stmt;
struct Program;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

struct Node {
  virtual ~Node() = default;
};

struct Expr : Node {
  enum class Kind {
    Number,
    String,
    Bool,
    Variable,
    List,
    Attribute,
    Unary,
    Binary,
    Call,
    Index,
  };

  Kind kind;
  explicit Expr(Kind kind) : kind(kind) {}
};

struct NumberExpr : Expr {
  double value;
  bool is_int;
  std::string raw_text;

  NumberExpr(double v, bool is_int_value, std::string raw = "")
      : Expr(Kind::Number), value(v), is_int(is_int_value), raw_text(std::move(raw)) {}
};

struct StringExpr : Expr {
  std::string value;

  explicit StringExpr(std::string text)
      : Expr(Kind::String), value(std::move(text)) {}
};

struct BoolExpr : Expr {
  bool value;

  explicit BoolExpr(bool v) : Expr(Kind::Bool), value(v) {}
};

struct VariableExpr : Expr {
  std::string name;

  explicit VariableExpr(std::string value) : Expr(Kind::Variable), name(std::move(value)) {}
};

struct ListExpr : Expr {
  std::vector<ExprPtr> elements;

  explicit ListExpr(std::vector<ExprPtr> elems) : Expr(Kind::List), elements(std::move(elems)) {}
};

enum class UnaryOp {
  Neg,
  Not,
  Await,
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;

  UnaryExpr(UnaryOp unary_op, ExprPtr child)
      : Expr(Kind::Unary), op(unary_op), operand(std::move(child)) {}
};

enum class BinaryOp {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Pow,
  Eq,
  Ne,
  Lt,
  Lte,
  Gt,
  Gte,
  And,
  Or,
};

struct BinaryExpr : Expr {
  BinaryOp op;
  ExprPtr left;
  ExprPtr right;

  BinaryExpr(BinaryOp binary_op, ExprPtr lhs, ExprPtr rhs)
      : Expr(Kind::Binary), op(binary_op), left(std::move(lhs)), right(std::move(rhs)) {}
};

struct CallExpr : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;

  CallExpr(ExprPtr callee_expr, std::vector<ExprPtr> call_args)
      : Expr(Kind::Call), callee(std::move(callee_expr)), args(std::move(call_args)) {}
};

struct AttributeExpr : Expr {
  ExprPtr target;
  std::string attribute;

  AttributeExpr(ExprPtr target_expr, std::string attr)
      : Expr(Kind::Attribute), target(std::move(target_expr)), attribute(std::move(attr)) {}
};

struct IndexExpr : Expr {
  ExprPtr target;
  struct IndexItem {
    bool is_slice = false;
    ExprPtr index;
    ExprPtr slice_start;
    ExprPtr slice_stop;
    ExprPtr slice_step;
  };
  std::vector<IndexItem> indices;

  IndexExpr(ExprPtr target_expr, std::vector<IndexItem> index_items)
      : Expr(Kind::Index), target(std::move(target_expr)), indices(std::move(index_items)) {}
};

struct Stmt : Node {
  enum class Kind {
    Expression,
    Assign,
    Return,
    Break,
    Continue,
    If,
    Switch,
    TryCatch,
    While,
    For,
    FunctionDef,
    ClassDef,
    WithTaskGroup,
  };

  Kind kind;
  explicit Stmt(Kind k) : kind(k) {}
};

struct ExpressionStmt : Stmt {
  ExprPtr expression;

  explicit ExpressionStmt(ExprPtr value)
      : Stmt(Kind::Expression), expression(std::move(value)) {}
};

struct AssignStmt : Stmt {
  ExprPtr target;
  ExprPtr value;

  AssignStmt(ExprPtr target_expr, ExprPtr rhs)
      : Stmt(Kind::Assign), target(std::move(target_expr)), value(std::move(rhs)) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value;

  explicit ReturnStmt(ExprPtr result) : Stmt(Kind::Return), value(std::move(result)) {}
};

struct BreakStmt : Stmt {
  BreakStmt() : Stmt(Kind::Break) {}
};

struct ContinueStmt : Stmt {
  ContinueStmt() : Stmt(Kind::Continue) {}
};

struct IfStmt : Stmt {
  ExprPtr condition;
  StmtList then_body;
  std::vector<std::pair<ExprPtr, StmtList>> elif_branches;
  StmtList else_body;

  IfStmt(ExprPtr cond, StmtList then, std::vector<std::pair<ExprPtr, StmtList>> elifs, StmtList else_body)
      : Stmt(Kind::If), condition(std::move(cond)), then_body(std::move(then)),
        elif_branches(std::move(elifs)), else_body(std::move(else_body)) {}
};

struct SwitchCase {
  ExprPtr match;
  StmtList body;

  SwitchCase(ExprPtr match_expr, StmtList case_body)
      : match(std::move(match_expr)), body(std::move(case_body)) {}
};

struct SwitchStmt : Stmt {
  ExprPtr selector;
  std::vector<SwitchCase> cases;
  StmtList default_body;

  SwitchStmt(ExprPtr selector_expr, std::vector<SwitchCase> switch_cases, StmtList default_block)
      : Stmt(Kind::Switch), selector(std::move(selector_expr)), cases(std::move(switch_cases)),
        default_body(std::move(default_block)) {}
};

struct TryCatchStmt : Stmt {
  StmtList try_body;
  std::string catch_name;
  StmtList catch_body;

  TryCatchStmt(StmtList try_block, std::string catch_var, StmtList catch_block)
      : Stmt(Kind::TryCatch), try_body(std::move(try_block)), catch_name(std::move(catch_var)),
        catch_body(std::move(catch_block)) {}
};

struct WhileStmt : Stmt {
  ExprPtr condition;
  StmtList body;

  WhileStmt(ExprPtr cond, StmtList loop_body)
      : Stmt(Kind::While), condition(std::move(cond)), body(std::move(loop_body)) {}
};

struct ForStmt : Stmt {
  std::string name;
  ExprPtr iterable;
  bool is_async = false;
  StmtList body;

  ForStmt(std::string target_name, ExprPtr it_expr, StmtList loop_body, bool async_loop = false)
      : Stmt(Kind::For), name(std::move(target_name)), iterable(std::move(it_expr)),
        is_async(async_loop), body(std::move(loop_body)) {}
};

struct FunctionDefStmt : Stmt {
  std::string name;
  std::vector<std::string> params;
  bool is_async = false;
  StmtList body;

  FunctionDefStmt(std::string name_value, std::vector<std::string> parameters,
                  bool async_flag, StmtList block)
      : Stmt(Kind::FunctionDef), name(std::move(name_value)), params(std::move(parameters)),
        is_async(async_flag), body(std::move(block)) {}
};

struct ClassDefStmt : Stmt {
  std::string name;
  bool open_shape = false;
  StmtList body;

  ClassDefStmt(std::string name_value, bool open, StmtList block)
      : Stmt(Kind::ClassDef), name(std::move(name_value)), open_shape(open), body(std::move(block)) {}
};

struct WithTaskGroupStmt : Stmt {
  std::string name;
  ExprPtr timeout_ms;
  StmtList body;

  WithTaskGroupStmt(std::string group_name, ExprPtr timeout_expr, StmtList block)
      : Stmt(Kind::WithTaskGroup), name(std::move(group_name)),
        timeout_ms(std::move(timeout_expr)), body(std::move(block)) {}
};

struct Program {
  StmtList body;

  explicit Program(StmtList stmts = {}) : body(std::move(stmts)) {}
};

// Pretty printer for debugging and phase reporting.
std::string to_source(const Program& program);

}  // namespace spark
