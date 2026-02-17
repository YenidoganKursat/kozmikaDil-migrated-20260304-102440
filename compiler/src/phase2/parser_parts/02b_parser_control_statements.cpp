// Control-flow statements:
// - if/elif/else
// - while
// - for-in loops

#include "spark/parser.h"

namespace spark {

StmtPtr Parser::parse_if_statement(int indent, const std::string& line) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;
  auto header = trim_static(line.substr(3));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid if statement", line_text);
  }
  header.pop_back();
  header = trim_static(header);
  if (header.empty()) {
    throw parse_error(line_no, "empty if condition", line_text);
  }

  auto cond = parse_expression(header);
  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "if body missing indentation", line_text);
  }

  auto then_body = parse_block(lines[index].indent);

  std::vector<std::pair<ExprPtr, StmtList>> elif_branches;
  StmtList else_body;
  while (index < lines.size() && lines[index].indent == indent) {
    const auto& next = lines[index].text;
    if (next == "else:") {
      ++index;
      if (index >= lines.size() || lines[index].indent <= indent) {
        throw parse_error(lines[index - 1].line_no, "else body missing indentation", lines[index - 1].text);
      }
      else_body = parse_block(lines[index].indent);
      break;
    }
    if (next.rfind("elif ", 0) == 0 && next.back() == ':') {
      auto ehead = trim_static(next.substr(5));
      if (ehead.empty() || ehead.back() != ':') {
        throw parse_error(lines[index].line_no, "invalid elif statement", next);
      }
      ehead.pop_back();
      ehead = trim_static(ehead);
      if (ehead.empty()) {
        throw parse_error(lines[index].line_no, "empty elif condition", next);
      }
      ++index;
      if (index >= lines.size() || lines[index].indent <= indent) {
        throw parse_error(lines[index - 1].line_no, "elif body missing indentation", lines[index - 1].text);
      }
      auto econd = parse_expression(ehead);
      auto ebody = parse_block(lines[index].indent);
      elif_branches.push_back({std::move(econd), std::move(ebody)});
      continue;
    }
    break;
  }
  return std::make_unique<IfStmt>(std::move(cond), std::move(then_body), std::move(elif_branches), std::move(else_body));
}

StmtPtr Parser::parse_while_statement(int indent, const std::string& line) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;
  auto header = trim_static(line.substr(6));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid while statement", line_text);
  }
  header.pop_back();
  if (header.empty()) {
    throw parse_error(line_no, "empty while condition", line_text);
  }
  ++index;

  auto cond = parse_expression(trim_static(header));
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "while body missing indentation", line_text);
  }
  auto body = parse_block(lines[index].indent);
  return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
}

StmtPtr Parser::parse_for_statement(int indent, const std::string& line, bool is_async) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;
  const auto prefix_len = is_async ? std::string("async for ").size() : std::string("for ").size();
  auto header = trim_static(line.substr(prefix_len));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid for statement", line_text);
  }
  header.pop_back();
  const auto in_pos = header.find(" in ");
  if (in_pos == std::string::npos) {
    throw parse_error(line_no, "missing 'in' in for statement", line_text);
  }

  const auto var_name = trim_static(header.substr(0, in_pos));
  if (!is_identifier_token(var_name)) {
    throw parse_error(line_no, "invalid loop variable", line_text);
  }

  auto iterable = parse_expression(trim_static(header.substr(in_pos + 4)));
  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "for body missing indentation", line_text);
  }
  auto body = parse_block(lines[index].indent);
  return std::make_unique<ForStmt>(var_name, std::move(iterable), std::move(body), is_async);
}

StmtPtr Parser::parse_with_task_group_statement(int indent, const std::string& line) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;

  constexpr std::string_view prefix = "with task_group";
  auto header = trim_static(line.substr(prefix.size()));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid with task_group statement", line_text);
  }
  header.pop_back();
  header = trim_static(header);

  ExprPtr timeout_expr = nullptr;
  if (!header.empty() && header[0] == '(') {
    std::size_t close = std::string::npos;
    int depth = 0;
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (header[i] == '(') {
        depth += 1;
      } else if (header[i] == ')') {
        depth -= 1;
        if (depth == 0) {
          close = i;
          break;
        }
      }
    }
    if (close == std::string::npos || depth != 0) {
      throw parse_error(line_no, "with task_group timeout missing ')'", line_text);
    }
    const auto timeout_src = trim_static(header.substr(1, close - 1));
    if (timeout_src.empty()) {
      throw parse_error(line_no, "with task_group timeout expression is empty", line_text);
    }
    timeout_expr = parse_expression(timeout_src);
    header = trim_static(header.substr(close + 1));
  }

  if (header.rfind("as ", 0) != 0) {
    throw parse_error(line_no, "with task_group must declare 'as <name>'", line_text);
  }
  const auto name = trim_static(header.substr(3));
  if (!is_identifier_token(name)) {
    throw parse_error(line_no, "invalid task_group variable name", line_text);
  }

  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "with task_group body missing indentation", line_text);
  }
  auto body = parse_block(lines[index].indent);
  return std::make_unique<WithTaskGroupStmt>(name, std::move(timeout_expr), std::move(body));
}

}  // namespace spark
