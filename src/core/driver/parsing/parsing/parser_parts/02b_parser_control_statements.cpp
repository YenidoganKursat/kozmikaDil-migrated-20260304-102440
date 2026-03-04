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

StmtPtr Parser::parse_switch_statement(int indent, const std::string& line) {
  const auto line_no = lines[index].line_no;
  const auto line_text = lines[index].text;
  auto header = trim_static(line.substr(7));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid switch statement", line_text);
  }
  header.pop_back();
  header = trim_static(header);
  if (header.empty()) {
    throw parse_error(line_no, "empty switch selector", line_text);
  }

  auto selector = parse_expression(header);
  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "switch body missing indentation", line_text);
  }
  const int clause_indent = lines[index].indent;

  std::vector<SwitchCase> switch_cases;
  StmtList default_body;
  bool has_default = false;

  while (index < lines.size() && lines[index].indent == clause_indent) {
    const auto clause_no = lines[index].line_no;
    const auto clause_text = lines[index].text;
    if (clause_text.rfind("case ", 0) == 0 && !clause_text.empty() && clause_text.back() == ':') {
      if (has_default) {
        throw parse_error(clause_no, "case cannot appear after default", clause_text);
      }
      auto case_src = trim_static(clause_text.substr(5));
      case_src.pop_back();
      case_src = trim_static(case_src);
      if (case_src.empty()) {
        throw parse_error(clause_no, "empty case expression", clause_text);
      }
      auto case_expr = parse_expression(case_src);
      ++index;
      if (index >= lines.size() || lines[index].indent <= clause_indent) {
        throw parse_error(clause_no, "case body missing indentation", clause_text);
      }
      auto case_body = parse_block(lines[index].indent);
      switch_cases.emplace_back(std::move(case_expr), std::move(case_body));
      continue;
    }
    if (clause_text == "default:") {
      if (has_default) {
        throw parse_error(clause_no, "duplicate default clause", clause_text);
      }
      has_default = true;
      ++index;
      if (index >= lines.size() || lines[index].indent <= clause_indent) {
        throw parse_error(clause_no, "default body missing indentation", clause_text);
      }
      default_body = parse_block(lines[index].indent);
      continue;
    }
    throw parse_error(clause_no, "switch supports only case/default clauses", clause_text);
  }

  if (switch_cases.empty() && !has_default) {
    throw parse_error(line_no, "switch requires at least one case/default clause", line_text);
  }

  return std::make_unique<SwitchStmt>(std::move(selector), std::move(switch_cases), std::move(default_body));
}

StmtPtr Parser::parse_try_catch_statement(int indent, const std::string& line) {
  const auto line_no = lines[index].line_no;
  const auto line_text = lines[index].text;
  if (line != "try:") {
    throw parse_error(line_no, "invalid try statement", line_text);
  }

  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "try body missing indentation", line_text);
  }
  auto try_body = parse_block(lines[index].indent);

  if (index >= lines.size() || lines[index].indent != indent) {
    throw parse_error(line_no, "try statement requires catch clause", line_text);
  }
  const auto catch_no = lines[index].line_no;
  const auto catch_text = lines[index].text;

  std::string catch_name;
  if (catch_text == "catch:") {
    // unnamed catch
  } else if (catch_text.rfind("catch as ", 0) == 0 && !catch_text.empty() && catch_text.back() == ':') {
    auto name = trim_static(catch_text.substr(9));
    name.pop_back();
    name = trim_static(name);
    if (!is_identifier_token(name)) {
      throw parse_error(catch_no, "invalid catch variable name", catch_text);
    }
    catch_name = std::move(name);
  } else {
    throw parse_error(catch_no, "invalid catch clause; use 'catch:' or 'catch as <name>:'", catch_text);
  }

  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(catch_no, "catch body missing indentation", catch_text);
  }
  auto catch_body = parse_block(lines[index].indent);

  return std::make_unique<TryCatchStmt>(std::move(try_body), std::move(catch_name), std::move(catch_body));
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
