// Definition statements:
// - function definitions with simple comma-separated parameter lists
// - class declarations with optional (open)/(slots) form

#include "spark/parser.h"

namespace spark {

StmtPtr Parser::parse_function_statement(int indent, const std::string& line, bool is_async) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;
  std::size_t prefix_len = std::string("def ").size();
  if (is_async) {
    if (line.rfind("async def ", 0) == 0) {
      prefix_len = std::string("async def ").size();
    } else if (line.rfind("async fn ", 0) == 0) {
      prefix_len = std::string("async fn ").size();
    } else {
      throw parse_error(line_no, "invalid async function declaration", line_text);
    }
  }
  auto header = trim_static(line.substr(prefix_len));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid function declaration", line_text);
  }
  header.pop_back();

  auto open = header.find('(');
  auto close = header.find(')');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    throw parse_error(line_no, "invalid function signature", line_text);
  }

  std::string name = trim_static(header.substr(0, open));
  if (!is_identifier_token(name)) {
    throw parse_error(line_no, "invalid function name", line_text);
  }

  std::vector<std::string> params;
  const std::string params_src = trim_static(header.substr(open + 1, close - open - 1));
  if (!params_src.empty()) {
    std::size_t pos = 0;
    while (true) {
      auto next = params_src.find(',', pos);
      std::string param = trim_static(params_src.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
      if (!is_identifier_token(param)) {
        throw parse_error(line_no, "invalid function parameter", line_text);
      }
      params.push_back(param);
      if (next == std::string::npos) {
        break;
      }
      pos = next + 1;
    }
  }

  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "function body missing indentation", line_text);
  }
  auto body = parse_block(lines[index].indent);
  return std::make_unique<FunctionDefStmt>(std::move(name), std::move(params), is_async, std::move(body));
}

StmtPtr Parser::parse_async_function_statement(int indent, const std::string& line) {
  return parse_function_statement(indent, line, true);
}

StmtPtr Parser::parse_class_statement(int indent, const std::string& line) {
  auto line_no = lines[index].line_no;
  auto line_text = lines[index].text;
  auto header = trim_static(line.substr(5));
  if (header.empty() || header.back() != ':') {
    throw parse_error(line_no, "invalid class declaration", line_text);
  }
  header.pop_back();

  const auto open = header.find('(');
  bool is_open_class = false;
  std::string name = header;
  if (open != std::string::npos) {
    const auto close = header.find(')');
    if (close == std::string::npos || close <= open + 1) {
      throw parse_error(line_no, "invalid class declaration", line_text);
    }
    const auto flag = trim_static(header.substr(open + 1, close - open - 1));
    if (flag == "open") {
      is_open_class = true;
    } else if (flag == "slots") {
      is_open_class = false;
    } else {
      throw parse_error(line_no, "unsupported class variant; use (open) or (slots)", line_text);
    }
    name = trim_static(header.substr(0, open));
    if (name.empty()) {
      throw parse_error(line_no, "invalid class name", line_text);
    }
    if (close != header.size() - 1) {
      throw parse_error(line_no, "invalid class declaration", line_text);
    }
  }

  name = trim_static(name);
  if (!is_identifier_token(name)) {
    throw parse_error(line_no, "invalid class name", line_text);
  }

  ++index;
  if (index >= lines.size() || lines[index].indent <= indent) {
    throw parse_error(line_no, "class body missing indentation", line_text);
  }
  auto body = parse_block(lines[index].indent);
  return std::make_unique<ClassDefStmt>(std::move(name), is_open_class, std::move(body));
}

}  // namespace spark
