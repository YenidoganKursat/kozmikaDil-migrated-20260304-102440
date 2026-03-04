// Statement dispatch entry:
// - chooses statement kind by prefix/shape
// - handles return statements and simple statement forms (assignment / expression)

#include "spark/parser.h"

namespace spark {

StmtPtr Parser::parse_statement(int indent) {
  if (index >= lines.size()) {
    throw parse_error(-1, "unexpected end of input");
  }

  const auto& line = lines[index].text;
  const auto& line_no = lines[index].line_no;
  const auto& line_text = lines[index].text;

  if (line.rfind("class ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_class_statement(indent, line);
  }
  if ((line.rfind("async def ", 0) == 0 || line.rfind("async fn ", 0) == 0) &&
      !line.empty() && line.back() == ':') {
    return parse_async_function_statement(indent, line);
  }
  if (line.rfind("async for ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_for_statement(indent, line, true);
  }
  if (line.rfind("def ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_function_statement(indent, line);
  }
  if (line.rfind("with task_group", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_with_task_group_statement(indent, line);
  }
  if (line.rfind("if ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_if_statement(indent, line);
  }
  if (line.rfind("switch ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_switch_statement(indent, line);
  }
  if (line == "try:") {
    return parse_try_catch_statement(indent, line);
  }
  if (line.rfind("while ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_while_statement(indent, line);
  }
  if (line.rfind("for ", 0) == 0 && !line.empty() && line.back() == ':') {
    return parse_for_statement(indent, line);
  }
  if (line == "break") {
    return parse_break_statement(line);
  }
  if (line == "continue") {
    return parse_continue_statement(line);
  }
  if (line == "return" || line.rfind("return ", 0) == 0) {
    return parse_return_statement(line);
  }
  if ((line.rfind("case ", 0) == 0 && !line.empty() && line.back() == ':') ||
      line == "default:" ||
      line == "catch:" ||
      (line.rfind("catch as ", 0) == 0 && !line.empty() && line.back() == ':')) {
    throw parse_error(line_no, "unexpected clause outside parent block", line_text);
  }

  if (line_no > 0) {
    try {
      return parse_assignment_or_expression(line);
    } catch (const ParseException& err) {
      const std::string msg = err.what();
      if (msg.rfind("line ", 0) == 0) {
        throw ParseException(err.what());
      }
      throw parse_error(line_no, msg, line_text);
    }
  }

  return parse_assignment_or_expression(line);
}

StmtPtr Parser::parse_return_statement(const std::string& line) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;
  if (!line.empty() && line != "return" && line.size() > 7 && !std::isspace(static_cast<unsigned char>(line[6]))) {
    throw parse_error(line_no, "invalid return syntax", line_text);
  }
  auto rhs = (line.size() <= 6) ? nullptr : parse_expression(trim_static(line.substr(7)));
  ++index;
  return std::make_unique<ReturnStmt>(std::move(rhs));
}

StmtPtr Parser::parse_break_statement(const std::string& line) {
  const auto line_no = lines[index].line_no;
  const auto line_text = lines[index].text;
  if (line != "break") {
    throw parse_error(line_no, "invalid break syntax", line_text);
  }
  ++index;
  return std::make_unique<BreakStmt>();
}

StmtPtr Parser::parse_continue_statement(const std::string& line) {
  const auto line_no = lines[index].line_no;
  const auto line_text = lines[index].text;
  if (line != "continue") {
    throw parse_error(line_no, "invalid continue syntax", line_text);
  }
  ++index;
  return std::make_unique<ContinueStmt>();
}

StmtPtr Parser::parse_assignment_or_expression(const std::string& line) {
  std::string lhs;
  std::string rhs;
  if (is_assignment(line, &lhs, &rhs)) {
    auto target = parse_expression(lhs);
    if (!is_assign_target_expr(*target)) {
      throw parse_error(lines[index].line_no, "invalid assignment target", lines[index].text);
    }
    auto value = parse_expression(rhs);
    ++index;
    return std::make_unique<AssignStmt>(std::move(target), std::move(value));
  }
  auto value = parse_expression(line);
  ++index;
  return std::make_unique<ExpressionStmt>(std::move(value));
}

}  // namespace spark
