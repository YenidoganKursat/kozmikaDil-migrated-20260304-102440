#include <cassert>
#include <string>
#include <unordered_map>

#include "spark/evaluator.h"
#include "spark/parser.h"

using namespace spark;

static void test_arithmetic_and_assign() {
  const std::string source = R"(
a = 1 + 2 * 3
b = (a + 4) - 5
c = a + b
)";
  Interpreter interpreter;
  Parser parser(source);
  interpreter.run(*parser.parse_program());
  auto globals = interpreter.snapshot_globals();

  assert(globals["a"].equals(Value::int_value_of(7)));
  assert(globals["b"].equals(Value::int_value_of(6)));
  assert(globals["c"].equals(Value::int_value_of(13)));
}

static void test_conditionals() {
  const std::string source = R"(
result = 0
if False:
  result = 10
elif 1 > 2:
  result = 20
else:
  result = 30
)";
  Interpreter interpreter;
  Parser parser(source);
  interpreter.run(*parser.parse_program());
  auto globals = interpreter.snapshot_globals();
  assert(globals["result"].equals(Value::int_value_of(30)));
}

static void test_loops() {
  const std::string source = R"(
sum = 0
i = 0
while i < 4:
  sum = sum + i
  i = i + 1

for v in range(3):
  sum = sum + v
)";
  Interpreter interpreter;
  Parser parser(source);
  interpreter.run(*parser.parse_program());
  auto globals = interpreter.snapshot_globals();
  assert(globals["sum"].equals(Value::int_value_of(9)));
}

static void test_functions() {
  const std::string source = R"(
def add(a, b):
  return a + b

x = 1
def scale(v):
  return add(v, x)

y = scale(4)
)";
  Interpreter interpreter;
  Parser parser(source);
  interpreter.run(*parser.parse_program());
  auto globals = interpreter.snapshot_globals();
  assert(globals["y"].equals(Value::int_value_of(5)));
}

static void test_lists_and_indexing() {
  const std::string source = R"(
matrix = [[1, 2], [3, 4]]
values = [10, 20, 30]
first = values[0]
second = matrix[1][0]
)";
  Interpreter interpreter;
  Parser parser(source);
  interpreter.run(*parser.parse_program());
  auto globals = interpreter.snapshot_globals();
  assert(globals["first"].equals(Value::int_value_of(10)));
  assert(globals["second"].equals(Value::int_value_of(3)));
}

int main() {
  test_arithmetic_and_assign();
  test_conditionals();
  test_loops();
  test_functions();
  test_lists_and_indexing();
  return 0;
}
