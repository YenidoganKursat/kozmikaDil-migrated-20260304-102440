#include <sstream>
#include <string>

#include "spark/ast.h"

namespace spark {

namespace {

std::string indent_line(std::size_t level) {
  return std::string(level * 2, ' ');
}

std::string escape_string_literal(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

int expr_precedence(const Expr& expr) {
  if (expr.kind == Expr::Kind::Binary) {
    const auto& binary = static_cast<const BinaryExpr&>(expr);
    switch (binary.op) {
      case BinaryOp::Or:
        return 1;
      case BinaryOp::And:
        return 2;
      case BinaryOp::Eq:
      case BinaryOp::Ne:
        return 3;
      case BinaryOp::Lt:
      case BinaryOp::Lte:
      case BinaryOp::Gt:
      case BinaryOp::Gte:
        return 4;
      case BinaryOp::Add:
      case BinaryOp::Sub:
        return 5;
      case BinaryOp::Mul:
      case BinaryOp::Div:
      case BinaryOp::Mod:
        return 6;
      case BinaryOp::Pow:
        return 7;
    }
  }
  if (expr.kind == Expr::Kind::Unary) {
    return 7;
  }
  if (expr.kind == Expr::Kind::Call || expr.kind == Expr::Kind::Index ||
      expr.kind == Expr::Kind::Attribute) {
    return 8;
  }
  return 9;
}

bool needs_parentheses(const Expr& expr, int parent_precedence) {
  return expr_precedence(expr) < parent_precedence;
}

std::string source_of_expr(const Expr& expr, std::size_t indent_level, int parent_precedence = 0);
std::string source_of_stmt(const Stmt& stmt, std::size_t indent_level);

std::string source_of_expr(const Expr& expr, std::size_t indent_level, int parent_precedence) {
  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& value_expr = static_cast<const NumberExpr&>(expr);
      if (value_expr.is_int) {
        return std::to_string(static_cast<long long>(value_expr.value));
      }
      std::ostringstream stream;
      stream << value_expr.value;
      return stream.str();
    }
    case Expr::Kind::String: {
      const auto& string_expr = static_cast<const StringExpr&>(expr);
      return "\"" + escape_string_literal(string_expr.value) + "\"";
    }
    case Expr::Kind::Bool:
      return static_cast<const BoolExpr&>(expr).value ? "True" : "False";
    case Expr::Kind::Variable:
      return static_cast<const VariableExpr&>(expr).name;
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      const char* op = unary.op == UnaryOp::Neg ? "-" : (unary.op == UnaryOp::Not ? "not " : "await ");
      // Pass a stronger parent precedence so children are not already wrapped twice.
      const std::string inner = source_of_expr(*unary.operand, indent_level, 0);
      if (needs_parentheses(*unary.operand, expr_precedence(expr))) {
        return std::string(op) + "(" + inner + ")";
      }
      return std::string(op) + inner;
    }
    case Expr::Kind::Attribute: {
      const auto& attribute = static_cast<const AttributeExpr&>(expr);
      return source_of_expr(*attribute.target, indent_level, 0) + "." + attribute.attribute;
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      const char* op = nullptr;
      switch (binary.op) {
        case BinaryOp::Add:
          op = "+"; break;
        case BinaryOp::Sub:
          op = "-"; break;
        case BinaryOp::Mul:
          op = "*"; break;
        case BinaryOp::Div:
          op = "/"; break;
        case BinaryOp::Mod:
          op = "%"; break;
        case BinaryOp::Pow:
          op = "^"; break;
        case BinaryOp::Eq:
          op = "=="; break;
        case BinaryOp::Ne:
          op = "!="; break;
        case BinaryOp::Lt:
          op = "<"; break;
        case BinaryOp::Lte:
          op = "<="; break;
        case BinaryOp::Gt:
          op = ">"; break;
        case BinaryOp::Gte:
          op = ">="; break;
        case BinaryOp::And:
          op = "and"; break;
        case BinaryOp::Or:
          op = "or"; break;
      }
      const int current_precedence = expr_precedence(expr);
      const std::string lhs = source_of_expr(*binary.left, indent_level, current_precedence);
      const std::string rhs = source_of_expr(*binary.right, indent_level, current_precedence + 1);
      const std::string full = lhs + " " + op + " " + rhs;
      return needs_parentheses(expr, parent_precedence) ? "(" + full + ")" : full;
    }
    case Expr::Kind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      std::string result = source_of_expr(*call.callee, indent_level, 0) + "(";
      for (std::size_t i = 0; i < call.args.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += source_of_expr(*call.args[i], indent_level, 0);
      }
      result += ")";
      return result;
    }
    case Expr::Kind::Index: {
      const auto& index = static_cast<const IndexExpr&>(expr);
      std::string out = source_of_expr(*index.target, indent_level, 0);
      for (std::size_t i = 0; i < index.indices.size(); ++i) {
        out += "[";
        const auto& item = index.indices[i];
        if (item.is_slice) {
          if (item.slice_start) {
            out += source_of_expr(*item.slice_start, indent_level, 0);
          }
          out += ":";
          if (item.slice_stop) {
            out += source_of_expr(*item.slice_stop, indent_level, 0);
          }
          if (item.slice_step) {
            out += ":" + source_of_expr(*item.slice_step, indent_level, 0);
          }
        } else if (item.index) {
          out += source_of_expr(*item.index, indent_level, 0);
        }
        out += "]";
      }
      return out;
    }
    case Expr::Kind::List: {
      const auto& list_expr = static_cast<const ListExpr&>(expr);
      std::string result = "[";
      for (std::size_t i = 0; i < list_expr.elements.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += source_of_expr(*list_expr.elements[i], indent_level, 0);
      }
      result += "]";
      return result;
    }
  }

  return "";
}

std::string source_of_stmt(const Stmt& stmt, std::size_t indent_level) {
  const std::string indent = indent_line(indent_level);

  switch (stmt.kind) {
    case Stmt::Kind::Expression: {
      const auto& expression = static_cast<const ExpressionStmt&>(stmt);
      return indent + source_of_expr(*expression.expression, indent_level, 0) + "\n";
    }
    case Stmt::Kind::Assign: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      return indent + source_of_expr(*assign.target, indent_level, 0) + " = " +
             source_of_expr(*assign.value, indent_level, 0) + "\n";
    }
    case Stmt::Kind::Return: {
      const auto& ret = static_cast<const ReturnStmt&>(stmt);
      if (ret.value) {
        return indent + "return " + source_of_expr(*ret.value, indent_level, 0) + "\n";
      }
      return indent + "return\n";
    }
    case Stmt::Kind::Break:
      return indent + "break\n";
    case Stmt::Kind::Continue:
      return indent + "continue\n";
    case Stmt::Kind::If: {
      const auto& ifs = static_cast<const IfStmt&>(stmt);
      std::string result = indent + "if " + source_of_expr(*ifs.condition, indent_level) + ":\n";
      for (const auto& child : ifs.then_body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      for (const auto& elif_pair : ifs.elif_branches) {
        result += indent + "elif " + source_of_expr(*elif_pair.first, indent_level) + ":\n";
        for (const auto& child : elif_pair.second) {
          result += source_of_stmt(*child, indent_level + 1);
        }
      }
      if (!ifs.else_body.empty()) {
      result += indent + "else:\n";
        for (const auto& child : ifs.else_body) {
          result += source_of_stmt(*child, indent_level + 1);
        }
      }
      return result;
    }
    case Stmt::Kind::Switch: {
      const auto& switch_stmt = static_cast<const SwitchStmt&>(stmt);
      std::string result = indent + "switch " + source_of_expr(*switch_stmt.selector, indent_level) + ":\n";
      for (const auto& switch_case : switch_stmt.cases) {
        result += indent + "  case " + source_of_expr(*switch_case.match, indent_level + 1, 0) + ":\n";
        for (const auto& child : switch_case.body) {
          result += source_of_stmt(*child, indent_level + 2);
        }
      }
      if (!switch_stmt.default_body.empty()) {
        result += indent + "  default:\n";
        for (const auto& child : switch_stmt.default_body) {
          result += source_of_stmt(*child, indent_level + 2);
        }
      }
      return result;
    }
    case Stmt::Kind::TryCatch: {
      const auto& try_stmt = static_cast<const TryCatchStmt&>(stmt);
      std::string result = indent + "try:\n";
      for (const auto& child : try_stmt.try_body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      if (try_stmt.catch_name.empty()) {
        result += indent + "catch:\n";
      } else {
        result += indent + "catch as " + try_stmt.catch_name + ":\n";
      }
      for (const auto& child : try_stmt.catch_body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      return result;
    }
    case Stmt::Kind::While: {
      const auto& while_stmt = static_cast<const WhileStmt&>(stmt);
      std::string result = indent + "while " + source_of_expr(*while_stmt.condition, indent_level) + ":\n";
      for (const auto& child : while_stmt.body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      return result;
    }
    case Stmt::Kind::For: {
      const auto& for_stmt = static_cast<const ForStmt&>(stmt);
      std::string result = indent + (for_stmt.is_async ? "async for " : "for ") + for_stmt.name + " in " +
                           source_of_expr(*for_stmt.iterable, indent_level) + ":\n";
      for (const auto& child : for_stmt.body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      return result;
    }
    case Stmt::Kind::FunctionDef: {
      const auto& fn = static_cast<const FunctionDefStmt&>(stmt);
      std::string result = indent + (fn.is_async ? "async def " : "def ") + fn.name + "(";
      for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i > 0) {
          result += ", ";
        }
        result += fn.params[i];
      }
      result += "):\n";
      for (const auto& child : fn.body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      return result;
    }
    case Stmt::Kind::ClassDef: {
      const auto& cls = static_cast<const ClassDefStmt&>(stmt);
      std::string result = indent + "class " + cls.name;
      if (cls.open_shape) {
        result += "(open)";
      }
      result += ":\n";
      for (const auto& child : cls.body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      return result;
    }
    case Stmt::Kind::WithTaskGroup: {
      const auto& with_stmt = static_cast<const WithTaskGroupStmt&>(stmt);
      std::string result = indent + "with task_group";
      if (with_stmt.timeout_ms) {
        result += "(" + source_of_expr(*with_stmt.timeout_ms, indent_level, 0) + ")";
      }
      result += " as " + with_stmt.name + ":\n";
      for (const auto& child : with_stmt.body) {
        result += source_of_stmt(*child, indent_level + 1);
      }
      return result;
    }
  }

  return "";
}

}  // namespace

std::string to_source(const Program& program) {
  std::string result;
  for (const auto& stmt : program.body) {
    result += source_of_stmt(*stmt, 0);
  }
  return result;
}

}  // namespace spark
