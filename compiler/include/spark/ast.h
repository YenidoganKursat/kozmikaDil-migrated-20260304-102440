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

  NumberExpr(double v, bool is_int_value) : Expr(Kind::Number), value(v), is_int(is_int_value) {}
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
    If,
    While,
    For,
    FunctionDef,
    ClassDef,
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

struct IfStmt : Stmt {
  ExprPtr condition;
  StmtList then_body;
  std::vector<std::pair<ExprPtr, StmtList>> elif_branches;
  StmtList else_body;

  IfStmt(ExprPtr cond, StmtList then, std::vector<std::pair<ExprPtr, StmtList>> elifs, StmtList else_body)
      : Stmt(Kind::If), condition(std::move(cond)), then_body(std::move(then)),
        elif_branches(std::move(elifs)), else_body(std::move(else_body)) {}
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
  StmtList body;

  ForStmt(std::string target_name, ExprPtr it_expr, StmtList loop_body)
      : Stmt(Kind::For), name(std::move(target_name)), iterable(std::move(it_expr)), body(std::move(loop_body)) {}
};

struct FunctionDefStmt : Stmt {
  std::string name;
  std::vector<std::string> params;
  StmtList body;

  FunctionDefStmt(std::string name_value, std::vector<std::string> parameters, StmtList block)
      : Stmt(Kind::FunctionDef), name(std::move(name_value)), params(std::move(parameters)),
        body(std::move(block)) {}
};

struct ClassDefStmt : Stmt {
  std::string name;
  bool open_shape = false;
  StmtList body;

  ClassDefStmt(std::string name_value, bool open, StmtList block)
      : Stmt(Kind::ClassDef), name(std::move(name_value)), open_shape(open), body(std::move(block)) {}
};

struct Program {
  StmtList body;

  explicit Program(StmtList stmts = {}) : body(std::move(stmts)) {}
};

// Pretty printer for debugging and phase reporting.
std::string to_source(const Program& program);

}  // namespace spark
