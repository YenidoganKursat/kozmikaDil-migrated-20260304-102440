#pragma once

#include <string>
#include <vector>

#include "spark/parser.h"

namespace spark {

class Lexer {
 public:
  Lexer(std::string text, int line_no = 0);

  std::vector<ExprToken> tokenize() const;

 private:
  std::string source;
  int line_no;
};

}  // namespace spark
