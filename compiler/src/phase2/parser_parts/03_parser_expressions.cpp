// Expression entry and operator-precedence parsing:
// - Pratt-like expression parser with precedence
// - tokenization via phase-2 lexer

#include "phase2/lexer.h"
#include "spark/parser.h"

namespace spark {

ExprPtr Parser::parse_expression(const std::string& text) {
  const auto line_no = index < lines.size() ? lines[index].line_no : -1;
  const auto line_text = index < lines.size() ? lines[index].text : std::string();
  try {
    auto tokens = tokenize_expression(text, static_cast<int>(line_no));
    std::size_t pos = 0;
    auto expr = parse_expr_binary(tokens, pos, 1);
    if (pos < tokens.size() && tokens[pos].type != ExprToken::Type::End) {
      throw parse_error(line_no, "unexpected token in expression: " + tokens[pos].text, line_text);
    }
    return expr;
  } catch (const ParseException& err) {
    const std::string message = err.what();
    if (line_no <= 0 || message.rfind("line ", 0) == 0) {
      throw;
    }
    throw parse_error(line_no, message, line_text);
  }
}

std::vector<ExprToken> Parser::tokenize_expression(const std::string& text, int line_no) {
  try {
    Lexer lexer(text, line_no);
    return lexer.tokenize();
  } catch (const ParseException& err) {
    if (line_no > 0) {
      throw parse_error(line_no, err.what());
    }
    throw;
  }
}

int Parser::precedence(const std::string& op) {
  if (op == "or") return 1;
  if (op == "and") return 2;
  if (op == "==" || op == "!=") return 3;
  if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
  if (op == "+" || op == "-") return 5;
  if (op == "*" || op == "/" || op == "%") return 6;
  if (op == "^") return 7;
  return -1;
}

ExprPtr Parser::parse_expr_binary(std::vector<ExprToken>& tokens, std::size_t& pos, int min_precedence) {
  auto lhs = parse_expr_unary(tokens, pos);
  while (pos < tokens.size()) {
    const auto& token = tokens[pos];
    if (token.type != ExprToken::Type::Operator) {
      break;
    }
    const int p = precedence(token.text);
    if (p < min_precedence || p < 1) {
      break;
    }
    const std::string op_text = token.text;
    ++pos;
    const int next_min_precedence = (op_text == "^") ? p : (p + 1);
    auto rhs = parse_expr_binary(tokens, pos, next_min_precedence);

    BinaryOp op = BinaryOp::Add;
    if (op_text == "+") op = BinaryOp::Add;
    else if (op_text == "-") op = BinaryOp::Sub;
    else if (op_text == "*") op = BinaryOp::Mul;
    else if (op_text == "/") op = BinaryOp::Div;
    else if (op_text == "%") op = BinaryOp::Mod;
    else if (op_text == "^") op = BinaryOp::Pow;
    else if (op_text == "==") op = BinaryOp::Eq;
    else if (op_text == "!=") op = BinaryOp::Ne;
    else if (op_text == "<") op = BinaryOp::Lt;
    else if (op_text == "<=") op = BinaryOp::Lte;
    else if (op_text == ">") op = BinaryOp::Gt;
    else if (op_text == ">=") op = BinaryOp::Gte;
    else if (op_text == "and") op = BinaryOp::And;
    else if (op_text == "or") op = BinaryOp::Or;

    lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

}  // namespace spark
