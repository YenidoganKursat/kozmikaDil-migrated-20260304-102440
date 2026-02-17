// Expression suffix parse:
// - function calls
// - indexing/slicing chains
// - attribute access chains
// - index item parser for x[i], x[i:j], x[:], etc.

#include "spark/parser.h"

namespace spark {

ExprPtr Parser::parse_expr_suffix(std::vector<ExprToken>& tokens, std::size_t& pos) {
  auto result = parse_expr_primary(tokens, pos);
  while (pos < tokens.size()) {
    if (tokens[pos].type == ExprToken::Type::LParen) {
      ++pos;
      std::vector<ExprPtr> args;
      if (pos < tokens.size() && tokens[pos].type != ExprToken::Type::RParen) {
        args.push_back(parse_expr_binary(tokens, pos, 1));
        while (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Comma) {
          ++pos;
          args.push_back(parse_expr_binary(tokens, pos, 1));
        }
      }
      if (pos >= tokens.size() || tokens[pos].type != ExprToken::Type::RParen) {
        throw parse_error(-1, "missing ) in function call");
      }
      ++pos;
      result = std::make_unique<CallExpr>(std::move(result), std::move(args));
      continue;
    }

    if (tokens[pos].type == ExprToken::Type::LBracket) {
      ++pos;
      std::vector<IndexExpr::IndexItem> indices;
      if (pos < tokens.size() && tokens[pos].type != ExprToken::Type::RBracket) {
        indices.push_back(parse_index_item(tokens, pos, -1));
        while (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Comma) {
          ++pos;
          if (pos >= tokens.size() || tokens[pos].type == ExprToken::Type::RBracket) {
            throw parse_error(-1, "empty index in indexing");
          }
          indices.push_back(parse_index_item(tokens, pos, -1));
        }
      }
      if (pos >= tokens.size() || tokens[pos].type != ExprToken::Type::RBracket) {
        throw parse_error(-1, "missing ] in indexing");
      }
      ++pos;
      result = std::make_unique<IndexExpr>(std::move(result), std::move(indices));
      continue;
    }

    if (tokens[pos].type == ExprToken::Type::Dot) {
      ++pos;
      if (pos >= tokens.size() || tokens[pos].type != ExprToken::Type::Identifier) {
        throw parse_error(-1, "expected attribute name after '.'");
      }
      const std::string attr_name = tokens[pos].text;
      if (attr_name.empty()) {
        throw parse_error(-1, "empty attribute name");
      }
      ++pos;
      result = std::make_unique<AttributeExpr>(std::move(result), attr_name);
      continue;
    }

    break;
  }

  return result;
}

IndexExpr::IndexItem Parser::parse_index_item(std::vector<ExprToken>& tokens, std::size_t& pos, int line_no) {
  IndexExpr::IndexItem item;
  if (pos >= tokens.size()) {
    throw parse_error(line_no, "invalid index expression");
  }

  if (tokens[pos].type == ExprToken::Type::Colon) {
    item.is_slice = true;
    ++pos;
  } else {
    auto first = parse_expr_binary(tokens, pos, 1);
    if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Colon) {
      item.is_slice = true;
      ++pos;
      item.slice_start = std::move(first);
    } else {
      item.index = std::move(first);
      return item;
    }
  }

  if (pos < tokens.size() && tokens[pos].type != ExprToken::Type::RBracket &&
      tokens[pos].type != ExprToken::Type::Comma &&
      tokens[pos].type != ExprToken::Type::Colon) {
    item.slice_stop = parse_expr_binary(tokens, pos, 1);
  }

  if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Colon) {
    ++pos;
    if (pos < tokens.size() && tokens[pos].type != ExprToken::Type::RBracket &&
        tokens[pos].type != ExprToken::Type::Comma) {
      item.slice_step = parse_expr_binary(tokens, pos, 1);
    }
  }

  if (pos < tokens.size() && tokens[pos].type == ExprToken::Type::Colon) {
    throw parse_error(line_no, "too many ':' in slice");
  }

  return item;
}

}  // namespace spark
