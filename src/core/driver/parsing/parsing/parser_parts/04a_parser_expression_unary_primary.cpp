// Expression leaf and unary parse:
// - unary prefix operators
// - grouped expressions
// - literals / identifiers / list or matrix literal construction

#include "spark/parser.h"

namespace spark {

namespace {

bool is_numeric_constructor_name(const std::string& name) {
  return name == "i8" || name == "i16" || name == "i32" || name == "i64" ||
         name == "i128" || name == "i256" || name == "i512" || name == "f8" ||
         name == "f16" || name == "bf16" || name == "f32" || name == "f64" ||
         name == "f128" || name == "f256" || name == "f512" || name == "string";
}

bool is_valid_prefix_ctor_argument_start(const ExprToken& token) {
  if (token.type == ExprToken::Type::Number || token.type == ExprToken::Type::String ||
      token.type == ExprToken::Type::Identifier || token.type == ExprToken::Type::LParen ||
      token.type == ExprToken::Type::LBracket) {
    return true;
  }
  if (token.type == ExprToken::Type::Operator) {
    return token.text == "-" || token.text == "not" || token.text == "await";
  }
  return false;
}

}  // namespace

ExprPtr Parser::parse_expr_unary(std::vector<ExprToken>& tokens, std::size_t& pos) {
  if (pos + 1 < tokens.size() && tokens[pos].type == ExprToken::Type::Identifier &&
      is_numeric_constructor_name(tokens[pos].text) &&
      tokens[pos + 1].type != ExprToken::Type::LParen &&
      is_valid_prefix_ctor_argument_start(tokens[pos + 1])) {
    const std::string ctor_name = tokens[pos].text;
    ++pos;
    auto arg = parse_expr_unary(tokens, pos);
    std::vector<ExprPtr> args;
    args.push_back(std::move(arg));
    return std::make_unique<CallExpr>(std::make_unique<VariableExpr>(ctor_name), std::move(args));
  }

  if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Operator) {
    const std::string op = tokens[pos].text;
    if (op == "-" || op == "not" || op == "await") {
      ++pos;
      UnaryOp uop = UnaryOp::Neg;
      if (op == "not") {
        uop = UnaryOp::Not;
      } else if (op == "await") {
        uop = UnaryOp::Await;
      }
      return std::make_unique<UnaryExpr>(uop, parse_expr_unary(tokens, pos));
    }
  }
  return parse_expr_suffix(tokens, pos);
}

ExprPtr Parser::parse_expr_primary(std::vector<ExprToken>& tokens, std::size_t& pos) {
  if (pos >= tokens.size()) {
    throw parse_error(-1, "unexpected end of expression");
  }

  const ExprToken& token = tokens[pos];
  ExprPtr result;

  if (token.type == ExprToken::Type::Number) {
    ++pos;
    const bool is_integer_literal = token.text.find('.') == std::string::npos &&
                                   token.text.find('e') == std::string::npos &&
                                   token.text.find('E') == std::string::npos;
    result = std::make_unique<NumberExpr>(std::stod(token.text), is_integer_literal, token.text);
  } else if (token.type == ExprToken::Type::String) {
    ++pos;
    result = std::make_unique<StringExpr>(token.text);
  } else if (token.type == ExprToken::Type::Identifier) {
    ++pos;
    if (token.text == "True") {
      result = std::make_unique<BoolExpr>(true);
    } else if (token.text == "False") {
      result = std::make_unique<BoolExpr>(false);
    } else {
      result = std::make_unique<VariableExpr>(token.text);
    }
  } else if (token.type == ExprToken::Type::LParen) {
    ++pos;
    result = parse_expr_binary(tokens, pos, 1);
    if (pos >= tokens.size() || tokens[pos].type != ExprToken::Type::RParen) {
      throw parse_error(-1, "missing ) in expression");
    }
    ++pos;
  } else if (token.type == ExprToken::Type::LBracket) {
    ++pos;
    std::vector<ExprPtr> elements;
    std::vector<ExprPtr> rows;
    bool matrix_mode = false;

    if (pos < tokens.size() && tokens[pos].type != ExprToken::Type::RBracket) {
      auto first = parse_expr_binary(tokens, pos, 1);
      if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Semicolon) {
        matrix_mode = true;
        if (first->kind != Expr::Kind::List) {
          throw parse_error(-1, "matrix row must be list literal");
        }
        rows.push_back(std::move(first));
        while (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Semicolon) {
          ++pos;
          auto row = parse_expr_binary(tokens, pos, 1);
          if (row->kind != Expr::Kind::List) {
            throw parse_error(-1, "matrix row must be list literal");
          }
          rows.push_back(std::move(row));
          if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Comma) {
            throw parse_error(-1, "comma cannot be mixed with semicolon matrix rows");
          }
        }
      } else {
        elements.push_back(std::move(first));
        while (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Comma) {
          ++pos;
          elements.push_back(parse_expr_binary(tokens, pos, 1));
          if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Semicolon) {
            throw parse_error(-1, "semicolon cannot be used in list literal without matrix row mode");
          }
        }
      }
    }

    if (pos >= tokens.size() || tokens[pos].type != ExprToken::Type::RBracket) {
      throw parse_error(-1, "missing ] in list literal");
    }
    ++pos;
    if (matrix_mode) {
      result = std::make_unique<ListExpr>(std::move(rows));
    } else {
      result = std::make_unique<ListExpr>(std::move(elements));
    }
  } else {
    throw parse_error(-1, "invalid token in expression");
  }

  return result;
}

}  // namespace spark
