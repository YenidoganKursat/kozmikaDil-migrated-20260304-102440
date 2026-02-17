#include <cassert>
#include <string>

#include "spark/ast.h"
#include "spark/parser.h"

namespace {

void assert_ast_snapshot(const std::string& source, const std::string& expected_source) {
  spark::Parser parser(source);
  auto program = parser.parse_program();
  const auto actual = spark::to_source(*program);
  assert(actual == expected_source);
}

void test_parser_assignment_and_expression() {
  assert_ast_snapshot(R"(
a = 1 + 2 * 3
a
)", "a = 1 + 2 * 3\na\n");
}

void test_parser_if_elif_else_blocks() {
  assert_ast_snapshot(R"(
if False:
  x = 1
elif True:
  x = 2
else:
  x = 3
)", "if False:\n  x = 1\nelif True:\n  x = 2\nelse:\n  x = 3\n");
}

void test_parser_for_while_and_return() {
  assert_ast_snapshot(R"(
while i < 3:
  i = i + 1
for v in range(3):
  print(v)
def f(x):
  return x
)", "while i < 3:\n  i = i + 1\nfor v in range(3):\n  print(v)\ndef f(x):\n  return x\n");
}

void test_parser_call_and_index_chain() {
  assert_ast_snapshot(R"(
x = values[1][0]
y = f(1, g(2 + 3))
)", "x = values[1][0]\ny = f(1, g(2 + 3))\n");
}

void test_parser_list_and_matrix_forms() {
  assert_ast_snapshot(R"(
values = [10, 20, 30]
matrix_a = [[1, 2], [3, 4]]
matrix_b = [[1,2];[3,4]]
)",
                     "values = [10, 20, 30]\nmatrix_a = [[1, 2], [3, 4]]\nmatrix_b = [[1, 2], [3, 4]]\n");
}

void test_parser_class_syntax() {
  assert_ast_snapshot(R"(
class Point:
  x = 1
  def add(a, b):
    return a + b
)", "class Point:\n  x = 1\n  def add(a, b):\n    return a + b\n");
}

void test_parser_parse_error_expected_failure() {
  bool failed = false;
  try {
    spark::Parser parser(R"(
if missing_condition
  x = 1
)");
    parser.parse_program();
  } catch (...) {
    failed = true;
  }
  assert(failed);
}

void test_parser_boolean_ops() {
  assert_ast_snapshot(R"(
ok = not False and True or True
)", "ok = not False and True or True\n");
}

void test_parser_unary_and_parentheses() {
  assert_ast_snapshot(R"(
v = -(3 + 4) * 2
)",
                     "v = -(3 + 4) * 2\n");
}

void test_parser_nested_functions() {
  assert_ast_snapshot(R"(
def outer():
  def inner(a, b):
    return a + b
  return inner(1, 2)
)",
                     "def outer():\n  def inner(a, b):\n    return a + b\n  return inner(1, 2)\n");
}

void test_parser_massive_assignment_snapshot() {
  std::string source;
  std::string expected;

  for (int i = 0; i < 120; ++i) {
    const int lhs = i;
    const int rhs_a = i;
    const int rhs_b = (i % 7) + 1;
    const int rhs_c = (i % 3) + 1;
    source += "x" + std::to_string(lhs) + " = " + std::to_string(rhs_a) + " + " + std::to_string(rhs_b) +
              " * " + std::to_string(rhs_c) + "\n";
    expected += "x" + std::to_string(lhs) + " = " + std::to_string(rhs_a) + " + " +
                std::to_string(rhs_b) + " * " + std::to_string(rhs_c) + "\n";
  }

  assert_ast_snapshot(source, expected);
}

}  // namespace

int main() {
  test_parser_assignment_and_expression();
  test_parser_if_elif_else_blocks();
  test_parser_for_while_and_return();
  test_parser_call_and_index_chain();
  test_parser_list_and_matrix_forms();
  test_parser_class_syntax();
  test_parser_parse_error_expected_failure();
  test_parser_boolean_ops();
  test_parser_unary_and_parentheses();
  test_parser_nested_functions();
  test_parser_massive_assignment_snapshot();
  return 0;
}
