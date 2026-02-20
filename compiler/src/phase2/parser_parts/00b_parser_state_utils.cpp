// Parser static helper utilities and state conversions:
// - parse error construction
// - assignment detection
// - identifier validation
// - indentation/comment helpers

#include <cctype>

#include "spark/parser.h"

namespace {

spark::ParseException parse_error(int line_no, const std::string& message, const std::string& line_text = "") {
  if (line_no <= 0) {
    return spark::ParseException(message);
  }
  std::string prefix = "line " + std::to_string(line_no) + ": " + message;
  if (!line_text.empty()) {
    return spark::ParseException(prefix + " | " + line_text);
  }
  return spark::ParseException(prefix);
}

bool is_assign_target_expr(const spark::Expr& expr) {
  if (expr.kind == spark::Expr::Kind::Variable) {
    return true;
  }
  if (expr.kind == spark::Expr::Kind::Index) {
    const auto& index = static_cast<const spark::IndexExpr&>(expr);
    return is_assign_target_expr(*index.target);
  }
  return false;
}

bool is_identifier_token(const std::string& token) {
  if (token.empty()) {
    return false;
  }
  if (!(std::isalpha(static_cast<unsigned char>(token[0])) || token[0] == '_')) {
    return false;
  }
  for (std::size_t i = 1; i < token.size(); ++i) {
    if (!(std::isalnum(static_cast<unsigned char>(token[i])) || token[i] == '_')) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace spark {

bool Parser::is_identifier(std::string_view value) {
  return is_identifier_token(std::string(value));
}

bool Parser::is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int Parser::string_prefix_indent(const std::string& value) {
  int spaces = 0;
  for (char c : value) {
    if (c == '\t') {
      spaces += 2;
      continue;
    }
    if (c == ' ') {
      ++spaces;
      continue;
    }
    break;
  }
  return spaces;
}

std::string Parser::strip_comment(std::string value) {
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    const bool escaped = i > 0 && value[i - 1] == '\\';
    if (!escaped && !in_double && ch == '\'') {
      in_single = !in_single;
      continue;
    }
    if (!escaped && !in_single && ch == '"') {
      in_double = !in_double;
      continue;
    }
    if (!in_single && !in_double && ch == '#') {
      return value.substr(0, i);
    }
  }
  return value;
}

ParseException Parser::error_at(int line_no, const std::string& message, const std::string& line_text) {
  return parse_error(line_no, message, line_text);
}

bool Parser::is_assignment(const std::string& line, std::string* target, std::string* rhs) {
  int depth = 0;
  bool in_single = false;
  bool in_double = false;
  std::size_t equals_pos = std::string::npos;

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    const bool escaped = i > 0 && line[i - 1] == '\\';
    if (!escaped && !in_double && ch == '\'') {
      in_single = !in_single;
      continue;
    }
    if (!escaped && !in_single && ch == '"') {
      in_double = !in_double;
      continue;
    }
    if (in_single || in_double) {
      continue;
    }

    if (ch == '(' || ch == '[') {
      ++depth;
      continue;
    }
    if (ch == ')' || ch == ']') {
      if (depth > 0) {
        --depth;
      }
      continue;
    }

    if (ch == '=' && depth == 0) {
      if (i + 1 < line.size() && line[i + 1] == '=') {
        ++i;
        continue;
      }
      if (i > 0 && (line[i - 1] == '<' || line[i - 1] == '>' || line[i - 1] == '!' || line[i - 1] == '=')) {
        continue;
      }
      equals_pos = i;
      break;
    }
  }

  if (equals_pos == std::string::npos) {
    return false;
  }

  const std::string lhs = trim_static(line.substr(0, equals_pos));
  const std::string rhs_text = trim_static(line.substr(equals_pos + 1));
  if (lhs.empty() || rhs_text.empty()) {
    return false;
  }

  *target = lhs;
  *rhs = rhs_text;
  return true;
}

std::string Parser::read_expression_slice(const std::string& line, std::size_t start) {
  auto text = trim_static(line.substr(start));
  return text;
}

}  // namespace spark
