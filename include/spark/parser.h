#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "spark/ast.h"

namespace spark {

struct ExprToken {
  enum class Type {
    Identifier,
    Number,
    String,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Semicolon,
    Dot,
    Comma,
    Colon,
    Operator,
    End,
  };

  Type type;
  std::string text;

  ExprToken(Type t = Type::End, std::string value = "")
      : type(t), text(std::move(value)) {}
};

struct ParseException : public std::runtime_error {
  explicit ParseException(std::string msg) : std::runtime_error(std::move(msg)) {}
};

class Parser {
 public:
  explicit Parser(std::string source_text);

  std::unique_ptr<Program> parse_program();

 private:
  std::string source;

  struct Line {
    int indent;
    int line_no;
    std::string text;
  };

  std::vector<Line> lines;
  std::size_t index = 0;

  void lex_lines();
  int current_indent() const;

  std::vector<StmtPtr> parse_block(int indent);
  StmtPtr parse_statement(int indent);
  StmtPtr parse_if_statement(int indent, const std::string& line);
  StmtPtr parse_switch_statement(int indent, const std::string& line);
  StmtPtr parse_try_catch_statement(int indent, const std::string& line);
  StmtPtr parse_while_statement(int indent, const std::string& line);
  StmtPtr parse_for_statement(int indent, const std::string& line, bool is_async = false);
  StmtPtr parse_function_statement(int indent, const std::string& line, bool is_async = false);
  StmtPtr parse_async_function_statement(int indent, const std::string& line);
  StmtPtr parse_class_statement(int indent, const std::string& line);
  StmtPtr parse_with_task_group_statement(int indent, const std::string& line);
  StmtPtr parse_return_statement(const std::string& line);
  StmtPtr parse_break_statement(const std::string& line);
  StmtPtr parse_continue_statement(const std::string& line);
  StmtPtr parse_assignment_or_expression(const std::string& line);

 	ExprPtr parse_expression(const std::string& text);

  static std::string trim(std::string value);
  static std::string trim_left(const std::string& value);
  static std::string trim_right(const std::string& value);
  static bool is_identifier(std::string_view value);
  static bool is_space(char c);
  static int string_prefix_indent(const std::string& value);
  static std::string strip_comment(std::string value);

		  ExprPtr parse_expr_binary(std::vector<ExprToken>& tokens, std::size_t& pos, int min_precedence);
		  ExprPtr parse_expr_unary(std::vector<ExprToken>& tokens, std::size_t& pos);
		  ExprPtr parse_expr_primary(std::vector<ExprToken>& tokens, std::size_t& pos);
		  ExprPtr parse_expr_suffix(std::vector<ExprToken>& tokens, std::size_t& pos);
		  IndexExpr::IndexItem parse_index_item(std::vector<ExprToken>& tokens, std::size_t& pos, int line_no);

  static std::vector<ExprToken> tokenize_expression(const std::string& text, int line_no = -1);
  static int precedence(const std::string& op);

  static bool is_assignment(const std::string& line, std::string* target, std::string* rhs);
  static std::string read_expression_slice(const std::string& line, std::size_t start);
  static ParseException error_at(int line_no, const std::string& message, const std::string& line_text = "");
};

}  // namespace spark
