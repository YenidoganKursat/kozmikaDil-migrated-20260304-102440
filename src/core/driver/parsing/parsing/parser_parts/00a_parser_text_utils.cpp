// Text and formatting helpers reused across parser entry points.
// Kept separate so parsing logic files remain focused on syntax shape.

#include <algorithm>
#include <cctype>
#include <sstream>

#include "spark/parser.h"

namespace {

std::string trim_left_static(std::string_view value) {
  std::size_t i = 0;
  while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' || value[i] == '\r')) {
    ++i;
  }
  return std::string(value.substr(i));
}

std::string trim_right_static(std::string_view value) {
  std::size_t end = value.size();
  while (end > 0 && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' || value[end - 1] == '\r')) {
    --end;
  }
  return std::string(value.substr(0, end));
}

std::string trim_static(std::string_view value) {
  return trim_right_static(trim_left_static(value));
}

std::vector<std::string> split_lines(const std::string& source) {
  std::vector<std::string> output;
  std::istringstream stream(source);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    output.push_back(line);
  }
  return output;
}

}  // namespace

namespace spark {

std::string Parser::trim(std::string value) {
  return trim_static(std::move(value));
}

std::string Parser::trim_left(const std::string& value) {
  return trim_left_static(std::string_view(value));
}

std::string Parser::trim_right(const std::string& value) {
  return trim_right_static(std::string_view(value));
}

}  // namespace spark
