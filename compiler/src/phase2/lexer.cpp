#include <cctype>
#include <stdexcept>

#include "lexer.h"

namespace spark {

namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_keyword(const std::string& token) {
  return token == "and" || token == "or" || token == "not" || token == "await";
}

bool is_integer_literal(const std::string& token) {
  if (token.empty()) {
    return false;
  }
  std::size_t i = 0;
  if (token[i] == '+' || token[i] == '-') {
    ++i;
  }
  if (i >= token.size()) {
    return false;
  }
  for (; i < token.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
      return false;
    }
  }
  return true;
}

char decode_string_escape(char escape) {
  switch (escape) {
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case '\\':
      return '\\';
    case '"':
      return '"';
    case '\'':
      return '\'';
    default:
      return escape;
  }
}

}  // namespace

Lexer::Lexer(std::string text, int line_no) : source(std::move(text)), line_no(line_no) {}

std::vector<ExprToken> Lexer::tokenize() const {
  std::vector<ExprToken> tokens;

  for (std::size_t i = 0; i < source.size();) {
    const char ch = source[i];
    if (is_space(ch)) {
      ++i;
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      std::size_t start = i;
      ++i;
      while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_')) {
        ++i;
      }
      const std::string token = source.substr(start, i - start);
      if (is_keyword(token)) {
        tokens.emplace_back(ExprToken::Type::Operator, token);
      } else {
        tokens.emplace_back(ExprToken::Type::Identifier, token);
      }
      continue;
    }

    if (ch == '"' || ch == '\'') {
      const char quote = ch;
      ++i;
      std::string decoded;
      bool closed = false;
      while (i < source.size()) {
        const char current = source[i++];
        if (current == '\\') {
          if (i >= source.size()) {
            throw ParseException("unterminated string escape");
          }
          decoded.push_back(decode_string_escape(source[i++]));
          continue;
        }
        if (current == quote) {
          closed = true;
          break;
        }
        decoded.push_back(current);
      }
      if (!closed) {
        throw ParseException("unterminated string literal");
      }
      tokens.emplace_back(ExprToken::Type::String, std::move(decoded));
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(ch)) || (ch == '.' && i + 1 < source.size() && std::isdigit(static_cast<unsigned char>(source[i + 1])))) {
      std::size_t start = i;
      ++i;
      bool has_dot = false;
      while (i < source.size()) {
        if (std::isdigit(static_cast<unsigned char>(source[i]))) {
          ++i;
          continue;
        }
        if (source[i] == '.' && !has_dot) {
          has_dot = true;
          ++i;
          continue;
        }
        break;
      }
      if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
        ++i;
        if (i < source.size() && (source[i] == '+' || source[i] == '-')) {
          ++i;
        }
        while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) {
          ++i;
        }
      }
      tokens.emplace_back(ExprToken::Type::Number, source.substr(start, i - start));
      continue;
    }

    if (ch == '(') {
      tokens.emplace_back(ExprToken::Type::LParen, "(");
      ++i;
      continue;
    }
    if (ch == ')') {
      tokens.emplace_back(ExprToken::Type::RParen, ")");
      ++i;
      continue;
    }
    if (ch == '[') {
      tokens.emplace_back(ExprToken::Type::LBracket, "[");
      ++i;
      continue;
    }
    if (ch == ']') {
      tokens.emplace_back(ExprToken::Type::RBracket, "]");
      ++i;
      continue;
    }
    if (ch == ',') {
      tokens.emplace_back(ExprToken::Type::Comma, ",");
      ++i;
      continue;
    }
    if (ch == ';') {
      tokens.emplace_back(ExprToken::Type::Semicolon, ";");
      ++i;
      continue;
    }
    if (ch == '.') {
      tokens.emplace_back(ExprToken::Type::Dot, ".");
      ++i;
      continue;
    }
    if (ch == ':') {
      tokens.emplace_back(ExprToken::Type::Colon, ":");
      ++i;
      continue;
    }

    if ((ch == '=' && i + 1 < source.size() && source[i + 1] == '=') ||
        (ch == '!' && i + 1 < source.size() && source[i + 1] == '=') ||
        (ch == '<' && i + 1 < source.size() && source[i + 1] == '=') ||
        (ch == '>' && i + 1 < source.size() && source[i + 1] == '=')) {
      tokens.emplace_back(ExprToken::Type::Operator, source.substr(i, 2));
      i += 2;
      continue;
    }

    if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '^' ||
        ch == '<' || ch == '>' || ch == '=') {
      tokens.emplace_back(ExprToken::Type::Operator, std::string(1, ch));
      ++i;
      continue;
    }

    throw ParseException("unexpected character in expression");
  }

  tokens.emplace_back(ExprToken::Type::End, "");
  return tokens;
}

}  // namespace spark
