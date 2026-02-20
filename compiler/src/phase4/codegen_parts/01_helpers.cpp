#include <iomanip>
#include <numeric>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <limits>
#include <regex>
#include <sstream>
#include <string_view>
#include <string>
#include <utility>

#include "spark/codegen.h"

namespace spark {

using Code = CodeGenerator::Code;

bool is_matrix_kind(ValueKind kind);
bool is_list_kind(ValueKind kind);

bool is_integer_constant_expr(const Expr& expr, long long& out) {
  if (expr.kind != Expr::Kind::Number) {
    return false;
  }
  const auto& number = static_cast<const NumberExpr&>(expr);
  if (!number.is_int) {
    return false;
  }
  out = static_cast<long long>(number.value);
  return true;
}

std::string trim_ws(const std::string& input) {
  auto start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  auto end = input.find_last_not_of(" \t\r\n");
  return input.substr(start, end - start + 1);
}

std::string regex_escape(const std::string& input) {
  static const std::string kSpecial = R"(.^$|()[]*+?{}\\\\)";
  std::string out;
  for (const char ch : input) {
    if (kSpecial.find(ch) != std::string::npos) {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  return out;
}

bool token_used_after(const std::vector<std::string>& lines, const std::string& token, std::size_t start_line) {
  if (token.empty()) {
    return false;
  }

  const auto pattern = std::string("(^|[^A-Za-z0-9_])") + regex_escape(token) + "([^A-Za-z0-9_]|$)";
  const std::regex token_re(pattern, std::regex::ECMAScript);

  for (std::size_t i = start_line; i < lines.size(); ++i) {
    const auto line = trim_ws(lines[i]);
    if (line.empty()) {
      continue;
    }
    if (std::regex_search(line, token_re)) {
      return true;
    }
  }
  return false;
}

std::size_t next_non_empty_line(const std::vector<std::string>& lines, std::size_t start) {
  for (std::size_t i = start; i < lines.size(); ++i) {
    if (!trim_ws(lines[i]).empty()) {
      return i;
    }
  }
  return lines.size();
}

bool is_identifier_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const auto ch : text) {
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  lines.push_back(current);
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::ostringstream out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out << lines[i];
    if (i + 1 < lines.size()) {
      out << "\n";
    }
  }
  return out.str();
}

bool is_temporary_name(const std::string& name) {
  return name.size() >= 3 && name[0] == '_' && name[1] == 't' && std::isdigit(static_cast<unsigned char>(name[2]));
}

void replace_identifier(std::string& line, const std::string& source, const std::string& replacement) {
  if (source.empty() || line.empty()) {
    return;
  }
  const auto src_len = source.size();
  for (std::size_t pos = 0; pos + src_len <= line.size();) {
    pos = line.find(source, pos);
    if (pos == std::string::npos) {
      return;
    }
    const bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
    const bool right_ok = pos + src_len >= line.size() || !is_identifier_char(line[pos + src_len]);
    if (left_ok && right_ok) {
      line.replace(pos, src_len, replacement);
      pos += replacement.size();
    } else {
      pos += src_len;
    }
  }
}

void collect_identifier_usage(const std::vector<std::string>& lines, std::unordered_map<std::string, std::size_t>& usage) {
  static const std::regex assignment_pattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*;?\s*$)");
  static const std::regex declaration_pattern(
      R"(^\s*(?:long long|double|long double|bool|int|__spark_string|__spark_list_i64\*|__spark_list_f64\*|__spark_matrix_i64\*|__spark_matrix_f64\*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
  static const std::regex identifier_pattern(R"([A-Za-z_][A-Za-z0-9_]*)");

  usage.clear();
  std::smatch match;
  for (const auto& raw : lines) {
    const auto line = trim_ws(raw);
    if (line.empty()) {
      continue;
    }
    if (std::regex_match(line, match, declaration_pattern)) {
      continue;
    }
    if (std::regex_match(line, match, assignment_pattern)) {
      const auto lhs = match[1].str();
      const auto rhs = match[2].str();
      auto it = std::sregex_iterator(rhs.begin(), rhs.end(), identifier_pattern);
      auto end = std::sregex_iterator();
      for (; it != end; ++it) {
        const auto token = it->str();
        ++usage[token];
      }
      if (lhs == "call" || lhs == "return" || lhs == "if" || lhs == "goto" || lhs == "for") {
        // best effort fallback; no special handling for these pseudo-IR patterns.
      }
      continue;
    }
    auto it = std::sregex_iterator(line.begin(), line.end(), identifier_pattern);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
      const auto token = it->str();
      ++usage[token];
    }
  }
}

std::unordered_set<std::string> collect_referenced_labels(const std::vector<std::string>& lines);
bool is_redundant_goto_to_next_label(const std::string& this_line, const std::string& next_line);

std::vector<std::string> canonicalize_c_lines(std::vector<std::string> lines) {
  static const std::regex assignment_pattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*;?\s*$)");
  static const std::regex declaration_pattern(
      R"(^\s*(?:long long|double|long double|bool|int|__spark_string|__spark_list_i64\*|__spark_list_f64\*|__spark_matrix_i64\*|__spark_matrix_f64\*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
  static const std::regex direct_call_pattern(R"(^[A-Za-z_][A-Za-z0-9_]*\s*\(.*\)$)");

  bool changed = true;
  while (changed) {
    changed = false;
    std::unordered_map<std::string, std::size_t> usage;
    collect_identifier_usage(lines, usage);

    for (std::size_t i = 0; i < lines.size(); ++i) {
      const auto raw = trim_ws(lines[i]);
      if (raw.empty()) {
        continue;
      }
      std::smatch match;
      if (!std::regex_match(raw, match, assignment_pattern)) {
        continue;
      }
      const auto lhs = match[1].str();
      auto rhs = trim_ws(match[2].str());
      if (!is_temporary_name(lhs)) {
        continue;
      }
      if (rhs.size() >= 2 && rhs.front() == '(' && rhs.back() == ')') {
        rhs = trim_ws(rhs.substr(1, rhs.size() - 2));
      }
      // Keep direct call temporaries as explicit locals so we do not
      // accidentally duplicate call evaluation by inlining into loop
      // conditions or branch expressions.
      if (std::regex_match(rhs, direct_call_pattern)) {
        continue;
      }
      if (usage[lhs] != 1) {
        continue;
      }
      for (auto& line : lines) {
        std::smatch decl_match;
        const auto line_trimmed_for_decl = trim_ws(line);
        if (std::regex_match(line_trimmed_for_decl, decl_match, declaration_pattern)) {
          continue;
        }
        if (line.empty()) {
          continue;
        }
        replace_identifier(line, lhs, rhs);
      }
      lines[i].clear();
      changed = true;
      break;
    }
  }

  std::unordered_map<std::string, std::size_t> usage;
  collect_identifier_usage(lines, usage);
  for (auto& line : lines) {
    std::smatch match;
    const auto line_trimmed = trim_ws(line);
    if (std::regex_match(line_trimmed, match, declaration_pattern)) {
      const auto name = match[1].str();
      if (is_temporary_name(name) && usage.find(name) == usage.end()) {
        line.clear();
      }
    }
  }

  const auto referenced_labels = collect_referenced_labels(lines);
  std::vector<std::string> label_pruned;
  label_pruned.reserve(lines.size());
  static const std::regex label_def_pattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*$)",
                                          std::regex::ECMAScript);
  std::smatch label_match;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto current_trimmed = trim_ws(lines[i]);
    if (current_trimmed.empty()) {
      continue;
    }
    if (is_redundant_goto_to_next_label(current_trimmed,
                                         (i + 1 < lines.size() ? trim_ws(lines[i + 1]) : ""))) {
      continue;
    }
    if (std::regex_match(current_trimmed, label_match, label_def_pattern)) {
      const auto label = label_match[1].str();
      if (referenced_labels.find(label) == referenced_labels.end()) {
        continue;
      }
    }
    label_pruned.push_back(lines[i]);
  }
  lines = std::move(label_pruned);

  std::vector<std::string> compacted;
  compacted.reserve(lines.size());
  for (auto& line : lines) {
    if (!trim_ws(line).empty()) {
      compacted.push_back(std::move(line));
    }
  }
  return compacted;
}

std::vector<const Stmt*> to_stmt_refs(const StmtList& block) {
  std::vector<const Stmt*> refs;
  refs.reserve(block.size());
  for (const auto& stmt : block) {
    refs.push_back(stmt.get());
  }
  return refs;
}

std::vector<std::string> split_csv_args(const std::string& input) {
  std::vector<std::string> args;
  int depth = 0;
  std::string current;
  for (char ch : input) {
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
    } else if (ch == ',' && depth == 0) {
      args.push_back(trim_ws(current));
      current.clear();
      continue;
    }
    current += ch;
  }
  const auto trimmed = trim_ws(current);
  if (!trimmed.empty()) {
    args.push_back(trimmed);
  }
  return args;
}

struct FlattenedIndexChain {
  const Expr* base = nullptr;
  std::vector<const IndexExpr::IndexItem*> indices;
};

std::string describe_expr(const Expr& expr);

std::string describe_expr_list(const std::vector<std::string>& values, const std::string& separator = ", ") {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += separator;
    }
    out += values[i];
  }
  return out;
}

std::string describe_expr(const Expr& expr) {
  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& number = static_cast<const NumberExpr&>(expr);
      return number.is_int ? std::to_string(static_cast<long long>(number.value)) : std::to_string(number.value);
    }
    case Expr::Kind::String: {
      const auto& string_expr = static_cast<const StringExpr&>(expr);
      return "\"" + string_expr.value + "\"";
    }
    case Expr::Kind::Bool: {
      return static_cast<const BoolExpr&>(expr).value ? "true" : "false";
    }
    case Expr::Kind::Variable: {
      return static_cast<const VariableExpr&>(expr).name;
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      const char* op = unary.op == UnaryOp::Neg ? "-" : (unary.op == UnaryOp::Not ? "not " : "await ");
      return std::string(op) + describe_expr(*unary.operand);
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      const char* op = " ";
      switch (binary.op) {
        case BinaryOp::Add: op = "+"; break;
        case BinaryOp::Sub: op = "-"; break;
        case BinaryOp::Mul: op = "*"; break;
        case BinaryOp::Div: op = "/"; break;
        case BinaryOp::Mod: op = "%"; break;
        case BinaryOp::Pow: op = "^"; break;
        case BinaryOp::Eq: op = "=="; break;
        case BinaryOp::Ne: op = "!="; break;
        case BinaryOp::Lt: op = "<"; break;
        case BinaryOp::Lte: op = "<="; break;
        case BinaryOp::Gt: op = ">"; break;
        case BinaryOp::Gte: op = ">="; break;
        case BinaryOp::And: op = "and"; break;
        case BinaryOp::Or: op = "or"; break;
      }
      return "(" + describe_expr(*binary.left) + " " + op + " " + describe_expr(*binary.right) + ")";
    }
    case Expr::Kind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      std::vector<std::string> args;
      args.reserve(call.args.size());
      for (const auto& arg : call.args) {
        args.push_back(describe_expr(*arg));
      }
      return describe_expr(*call.callee) + "(" + describe_expr_list(args) + ")";
    }
    case Expr::Kind::Index: {
      const auto& index = static_cast<const IndexExpr&>(expr);
      std::string target = describe_expr(*index.target);
      for (const auto& item : index.indices) {
        target += "[";
        if (item.is_slice) {
          if (item.slice_start) {
            target += describe_expr(*item.slice_start);
          }
          target += ":";
          if (item.slice_stop) {
            target += describe_expr(*item.slice_stop);
          }
          if (item.slice_step) {
            target += ":";
            target += describe_expr(*item.slice_step);
          }
        } else if (item.index) {
          target += describe_expr(*item.index);
        }
        target += "]";
      }
      return target;
    }
    case Expr::Kind::Attribute: {
      const auto& attr = static_cast<const AttributeExpr&>(expr);
      return describe_expr(*attr.target) + "." + attr.attribute;
    }
    case Expr::Kind::List: {
      const auto& list = static_cast<const ListExpr&>(expr);
      std::vector<std::string> values;
      values.reserve(list.elements.size());
      for (const auto& element : list.elements) {
        values.push_back(describe_expr(*element));
      }
      return "[" + describe_expr_list(values) + "]";
    }
  }

  return "<expr>";
}

const VariableExpr* root_variable_expr(const Expr& expr) {
  if (expr.kind == Expr::Kind::Variable) {
    return &static_cast<const VariableExpr&>(expr);
  }
  if (expr.kind == Expr::Kind::Index) {
    return root_variable_expr(*static_cast<const IndexExpr&>(expr).target);
  }
  return nullptr;
}

FlattenedIndexChain flatten_index_chain(const IndexExpr& index_expr) {
  std::vector<const IndexExpr*> chain;
  const Expr* current = &index_expr;
  while (current->kind == Expr::Kind::Index) {
    chain.push_back(static_cast<const IndexExpr*>(current));
    current = static_cast<const IndexExpr*>(current)->target.get();
  }

  FlattenedIndexChain out;
  out.base = current;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const auto* index_node = *it;
    for (const auto& item : index_node->indices) {
      out.indices.push_back(&item);
    }
  }
  return out;
}

ValueKind infer_ast_expr_kind(const Expr& expr) {
  switch (expr.kind) {
    case Expr::Kind::Number: {
      const auto& number = static_cast<const NumberExpr&>(expr);
      return number.is_int ? ValueKind::Int : ValueKind::Float;
    }
    case Expr::Kind::String:
      return ValueKind::String;
    case Expr::Kind::Bool:
      return ValueKind::Bool;
    case Expr::Kind::List:
      return ValueKind::ListAny;
    case Expr::Kind::Call:
    case Expr::Kind::Attribute:
    case Expr::Kind::Index:
    case Expr::Kind::Variable:
      return ValueKind::Unknown;
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      const auto inner = unary.operand.get();
      return inner ? infer_ast_expr_kind(*inner) : ValueKind::Unknown;
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      const auto left = infer_ast_expr_kind(*binary.left);
      const auto right = infer_ast_expr_kind(*binary.right);
      if (left == ValueKind::Invalid || right == ValueKind::Invalid) {
        return ValueKind::Invalid;
      }
      if (left == ValueKind::Unknown) {
        return right;
      }
      if (right == ValueKind::Unknown) {
        return left;
      }
      if (left == right) {
        return left;
      }
      if (left == ValueKind::Float || right == ValueKind::Float) {
        return ValueKind::Float;
      }
      if (left == ValueKind::Int && right == ValueKind::Bool) {
        return ValueKind::Int;
      }
      if (left == ValueKind::Bool && right == ValueKind::Int) {
        return ValueKind::Int;
      }
      return ValueKind::Unknown;
    }
  }
  return ValueKind::Unknown;
}

std::string value_type_for_identifier(const ValueKind kind) {
  switch (kind) {
    case ValueKind::Int:
      return "i64";
    case ValueKind::Float:
      return "f64";
    case ValueKind::Bool:
      return "bool";
    case ValueKind::String:
      return "str";
    case ValueKind::ListInt:
      return "list_i64";
    case ValueKind::ListFloat:
      return "list_f64";
    case ValueKind::ListAny:
      return "list_i64";
    case ValueKind::MatrixInt:
      return "matrix_i64";
    case ValueKind::MatrixFloat:
      return "matrix_f64";
    case ValueKind::MatrixAny:
      return "matrix_i64";
    case ValueKind::Unknown:
      return "unknown";
    case ValueKind::Void:
      return "void";
    case ValueKind::Invalid:
      return "invalid";
  }
  return "unknown";
}

ValueKind container_scalar_element_kind(ValueKind container_kind) {
  if (container_kind == ValueKind::ListFloat || container_kind == ValueKind::MatrixFloat) {
    return ValueKind::Float;
  }
  return ValueKind::Int;
}

ValueKind container_scalar_kind(ValueKind container_kind) {
  return container_scalar_element_kind(container_kind);
}

ValueKind normalize_index_kind(ValueKind kind) {
  return kind == ValueKind::Bool ? ValueKind::Int : kind;
}

ValueKind container_kind_for_index_base(ValueKind base_kind, ValueKind index_result_kind, bool list_expected) {
  if (list_expected) {
    return container_scalar_kind(base_kind);
  }
  if (index_result_kind == ValueKind::Invalid) {
    return ValueKind::Invalid;
  }
  if (is_list_kind(base_kind) || is_matrix_kind(base_kind)) {
    return container_scalar_element_kind(base_kind);
  }
  return index_result_kind;
}

std::vector<ValueKind> infer_matrix_row_kinds(const ListExpr& row) {
  std::vector<ValueKind> kinds;
  kinds.reserve(row.elements.size());
  for (const auto& element : row.elements) {
    kinds.push_back(normalize_index_kind(infer_ast_expr_kind(*element)));
  }
  return kinds;
