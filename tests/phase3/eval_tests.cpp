#include <cassert>
#include <sstream>
#include <string>
#include <vector>

#include "spark/evaluator.h"
#include "spark/parser.h"

namespace {

void run_program(const std::string& source, const std::string& expected_name, const spark::Value& expected) {
  spark::Interpreter interpreter;
  spark::Parser parser(source);
  auto program = parser.parse_program();
  auto result = interpreter.run(*program);
  (void)result;
  assert(interpreter.has_global(expected_name));
  const auto actual = interpreter.global(expected_name);
  assert(actual.equals(expected));
}

void run_scalar_programs() {
  run_program(R"(
x = 1
y = 2
z = x * y + 3
)", "z", spark::Value::int_value_of(5));

  run_program(R"(
a = 10.0
b = a / 4
c = b + 1.5
)", "c", spark::Value::double_value_of(4.0));
}

void run_bulk_arithmetic_suite() {
  for (int i = 0; i < 100; ++i) {
    std::ostringstream stream;
    stream << "x = " << i << "\n"
           << "y = " << (i + 3) << "\n"
           << "z = x + y + 1\n"
           << "q = x * 2\n"
           << "r = q - 1\n";

    run_program(stream.str(), "z", spark::Value::int_value_of(2 * i + 4));
    run_program(stream.str(), "q", spark::Value::int_value_of(2 * i));
    run_program(stream.str(), "r", spark::Value::int_value_of(2 * i - 1));
  }
}

void run_list_and_matrix_programs() {
  run_program(R"(
values = [10, 20, 30, 40]
first = values[0]
second = values[3]
)", "second", spark::Value::int_value_of(40));

  run_program(R"(
matrix = [[1, 2], [3, 4]]
diag = matrix[0][0] + matrix[1][1]
)", "diag", spark::Value::int_value_of(5));
}

void run_matrix_semicolon_program() {
  run_program(R"(
matrix = [[1, 2]; [3, 4]]
value = matrix[1][0]
)", "value", spark::Value::int_value_of(3));
}

void run_loops_and_conditionals() {
  run_program(R"(
i = 0
total = 0
while i < 5:
  total = total + i
  i = i + 1
)", "total", spark::Value::int_value_of(10));

  run_program(R"(
result = 0
if False:
  result = 1
elif 0 < 1:
  result = 99
else:
  result = 42
)", "result", spark::Value::int_value_of(99));
}

void run_for_and_range() {
  run_program(R"(
acc = 0
for v in range(4):
  acc = acc + v
)", "acc", spark::Value::int_value_of(6));
}

void run_functions() {
  run_program(R"(
def add(a, b):
  return a + b

def scale(v):
  return add(v, 3)

out = scale(7)
)", "out", spark::Value::int_value_of(10));
}

void run_boolean_logic() {
  run_program(R"(
a = not False and True
b = a or False
)", "b", spark::Value::bool_value_of(true));
}

void run_nested_calls() {
  run_program(R"(
def plus(a, b):
  return a + b
def twice(x):
  return x * 2
out = plus(twice(3), 1)
)", "out", spark::Value::int_value_of(7));
}

void run_parser_ast_smoke() {
  const std::string source = R"(
class A:
  x = 1
  def value():
    return x
)";
  spark::Interpreter interpreter;
  spark::Parser parser(source);
  auto program = parser.parse_program();
  interpreter.run(*program);
  assert(interpreter.has_global("A"));
}

void run_return_and_nested() {
  run_program(R"(
def outer(n):
  def inner(x):
    if x == 0:
      return 1
    return x
  return inner(n)

value = outer(0)
)", "value", spark::Value::int_value_of(1));
}

void run_list_addition() {
  run_program(R"(
left = [1, 2]
right = [3, 4]
both = left + right
)", "both", spark::Value::list_value_of({
  spark::Value::int_value_of(1),
  spark::Value::int_value_of(2),
  spark::Value::int_value_of(3),
  spark::Value::int_value_of(4),
}));
}

void run_matrix_builtin_constructor() {
  run_program(R"(
m = matrix_i64(2, 3)
m[1, 2] = 7
m[0, 1] = 5
value = m[1, 2] + m[0, 1]
)", "value", spark::Value::int_value_of(12));
}

}  // namespace

int main() {
  run_scalar_programs();
  run_bulk_arithmetic_suite();
  run_list_and_matrix_programs();
  run_matrix_semicolon_program();
  run_loops_and_conditionals();
  run_for_and_range();
  run_functions();
  run_boolean_logic();
  run_nested_calls();
  run_parser_ast_smoke();
  run_return_and_nested();
  run_list_addition();
  run_matrix_builtin_constructor();
  return 0;
}
