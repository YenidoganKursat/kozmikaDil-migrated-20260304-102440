// Parser core entry points:
// - parse_program orchestration
// - line tokenization and indentation traversal
// - shared block parsing with indentation validation

#include <utility>

#include "spark/parser.h"

namespace spark {

Parser::Parser(std::string source_text) : source(std::move(source_text)) {
  lex_lines();
}

std::unique_ptr<Program> Parser::parse_program() {
  index = 0;
  auto body = parse_block(0);
  if (index != lines.size()) {
    throw parse_error(lines[index].line_no, "unexpected trailing content");
  }
  return std::make_unique<Program>(Program{std::move(body)});
}

void Parser::lex_lines() {
  lines.clear();
  index = 0;

  const auto raw_lines = split_lines(source);
  for (std::size_t i = 0; i < raw_lines.size(); ++i) {
    auto line = strip_comment(raw_lines[i]);
    line = Parser::trim(line);
    if (line.empty()) {
      continue;
    }
    int indent = string_prefix_indent(raw_lines[i]);
    lines.push_back({indent, static_cast<int>(i + 1), std::move(line)});
  }
}

int Parser::current_indent() const {
  if (index >= lines.size()) {
    return 0;
  }
  return lines[index].indent;
}

std::vector<StmtPtr> Parser::parse_block(int indent) {
  std::vector<StmtPtr> result;
  while (index < lines.size()) {
    int line_indent = current_indent();
    if (line_indent < indent) {
      break;
    }
    if (line_indent > indent) {
      throw parse_error(lines[index].line_no, "unexpected indentation");
    }
    result.push_back(parse_statement(indent));
  }
  return result;
}

}  // namespace spark
