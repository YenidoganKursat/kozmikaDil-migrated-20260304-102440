#include <cassert>
#include <iostream>
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
  if (!actual.equals(expected)) {
    std::cerr << "Mismatch for variable '" << expected_name << "'\n";
    std::cerr << "Source:\n" << source << "\n";
    std::cerr << "Expected: " << expected.to_string() << "\n";
    std::cerr << "Actual:   " << actual.to_string() << "\n";
    assert(false);
  }
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

  run_program(R"(
p = 2 ^ 10
)", "p", spark::Value::double_value_of(1024.0));
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

  run_program(R"(
acc = 0
for v in range(4):
  acc = v + acc
)", "acc", spark::Value::int_value_of(6));

  run_program(R"(
acc = 0
for v in range(4):
  acc = acc - v
)", "acc", spark::Value::int_value_of(-6));
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

  run_program(R"(
m = matrix_f64(2, 2)
m[0, 0] = 1.5
m[1, 1] = 2.5
value = m[0, 0] + m[1, 1]
)", "value", spark::Value::double_value_of(4.0));
}

void run_affine_fill_builtin_constructor() {
  run_program(R"(
xs = list_fill_affine(6, 3, 1, 7, 0.5)
s = xs[0] + xs[1] + xs[2] + xs[3] + xs[4] + xs[5]
)", "s", spark::Value::int_value_of(8));

  run_program(R"(
m = matrix_fill_affine(2, 2, 3, 1, 7, 0.25)
v = m[0, 0] + m[1, 1]
)", "v", spark::Value::int_value_of(1));
}

void run_numeric_primitive_builtins() {
  run_program(R"(
x = f32(1.5)
y = f32(2.25)
z = x + y
)", "z", spark::Value::numeric_value_of(spark::Value::NumericKind::F32, "3.75"));

  run_program(R"(
a = f512(1.0)
b = f512(2.0)
c = a * b + a
)", "c", spark::Value::numeric_value_of(spark::Value::NumericKind::F512, "3"));

  run_program(R"(
p = i128(7)
q = i128(9)
r = p * q
)", "r", spark::Value::numeric_value_of(spark::Value::NumericKind::I128, "63"));

  run_program(R"(
a = f8 1.5
b = f16 2.5
c = f32 3.5
d = f128 4.5
e = f256 5.5
f = f512 6.5
sum = a + b + c + d + e + f
)", "sum", spark::Value::numeric_value_of(spark::Value::NumericKind::F512, "24"));

  run_program(R"(
x = f64(9.0)
y = f64(0.5)
z = x ^ y
)", "z", spark::Value::numeric_value_of(spark::Value::NumericKind::F64, "3"));

  run_program(R"(
x = f512 1
i = 0
while i < 200:
  x = x / f512 2
  i = i + 1
delta = (f512 1 + x) - (f512 1)
nz = delta != f512 0
)", "nz", spark::Value::bool_value_of(true));

  run_program(R"(
x = f128 1
i = 0
while i < 200:
  x = x / f128 2
  i = i + 1
delta = (f128 1 + x) - (f128 1)
nz = delta != f128 0
)", "nz", spark::Value::bool_value_of(false));

  run_program(R"(
x = f256 1
i = 0
while i < 200:
  x = x / f256 2
  i = i + 1
delta = (f256 1 + x) - (f256 1)
nz = delta != f256 0
)", "nz", spark::Value::bool_value_of(true));
}

void run_while_hotloop_numeric_fastpath_smoke() {
  run_program(R"(
i = 0
x = f128 1.25
step = f128 0.5
while i < 1000:
  x = x + step
  i = i + 1
)", "x", spark::Value::numeric_value_of(spark::Value::NumericKind::F128, "501.25"));

  run_program(R"(
i = 0
x = 0
while i < 5:
  x = x + i
  i = i + 1
)", "x", spark::Value::int_value_of(10));

  run_program(R"(
i = 0
x = 0
while i < 5:
  i = i + 1
  x = x + i
)", "x", spark::Value::int_value_of(15));

  run_program(R"(
i = 0
s = 0
p = 1
step = 3
while i < 12:
  s = s + i
  p = p * 2
  i = i + step
)", "s", spark::Value::int_value_of(18));

  run_program(R"(
i = 0
x = f512 1
y = f512 2
delta = f512 0.5
while i < 4:
  i = i + 1
  x = x + delta
  y = y * x
)", "y", spark::Value::numeric_value_of(spark::Value::NumericKind::F512, "45"));
}

void run_numeric_constructor_family_smoke() {
  struct PrimitiveCase {
    const char* ctor;
    spark::Value::NumericKind kind;
  };
  const std::vector<PrimitiveCase> cases = {
      {"i8", spark::Value::NumericKind::I8},     {"i16", spark::Value::NumericKind::I16},
      {"i32", spark::Value::NumericKind::I32},   {"i64", spark::Value::NumericKind::I64},
      {"i128", spark::Value::NumericKind::I128}, {"i256", spark::Value::NumericKind::I256},
      {"i512", spark::Value::NumericKind::I512}, {"f8", spark::Value::NumericKind::F8},
      {"f16", spark::Value::NumericKind::F16},   {"bf16", spark::Value::NumericKind::BF16},
      {"f32", spark::Value::NumericKind::F32},   {"f64", spark::Value::NumericKind::F64},
      {"f128", spark::Value::NumericKind::F128}, {"f256", spark::Value::NumericKind::F256},
      {"f512", spark::Value::NumericKind::F512},
  };

  for (const auto& primitive : cases) {
    std::ostringstream stream;
    stream << "v = " << primitive.ctor << "(7)\n";
    run_program(stream.str(), "v", spark::Value::numeric_value_of(primitive.kind, "7"));
  }
}

void run_string_primitive_programs() {
  run_program(R"(
a = "koz"
b = "mika"
c = a + b
)", "c", spark::Value::string_value_of("kozmika"));

  run_program(R"(
s = string(123)
)", "s", spark::Value::string_value_of("123"));

  run_program(R"(
t = "platform_support"
u = t[1:5]
)", "u", spark::Value::string_value_of("latf"));

  run_program(R"(
t = "kozmika"
v = t[3]
)", "v", spark::Value::string_value_of("m"));

  run_program(R"(
t = "kafe🙂"
u8 = utf8_len(t)
)", "u8", spark::Value::int_value_of(8));

  run_program(R"(
t = "kafe🙂"
u16 = utf16_len(t)
)", "u16", spark::Value::int_value_of(6));
}

void run_bench_tick_family_smoke() {
  run_program(R"(
n = bench_tick_scale_num()
d = bench_tick_scale_den()
t1 = bench_tick_raw()
t2 = bench_tick_raw()
ok = n > 0 and d > 0 and t2 >= t1
)", "ok", spark::Value::bool_value_of(true));

  run_program(R"(
t1 = bench_tick()
t2 = bench_tick()
ok = t2 >= t1
)", "ok", spark::Value::bool_value_of(true));
}

void run_switch_try_and_loop_control() {
  run_program(R"(
x = 3
y = 0
switch x:
  case 1:
    y = 10
  case 3:
    y = 30
  default:
    y = 99
)", "y", spark::Value::int_value_of(30));

  run_program(R"(
x = 1
y = 0
switch x:
  case 1:
    y = 5
    break
    y = 8
  default:
    y = 9
)", "y", spark::Value::int_value_of(5));

  run_program(R"(
acc = 0
for i in range(6):
  if i % 2 == 0:
    continue
  acc = acc + i
)", "acc", spark::Value::int_value_of(9));

  run_program(R"(
i = 0
acc = 0
while i < 10:
  i = i + 1
  if i == 4:
    break
  acc = acc + i
)", "acc", spark::Value::int_value_of(6));

  run_program(R"(
i = 0
acc = 0
while i < 5:
  i = i + 1
  if i < 3:
    continue
  acc = acc + 1
)", "acc", spark::Value::int_value_of(3));

  run_program(R"(
x = 0
try:
  y = 1 / 0
  x = 1
catch as err:
  x = 7
)", "x", spark::Value::int_value_of(7));

  run_program(R"(
x = 0
try:
  x = 5
catch:
  x = 9
)", "x", spark::Value::int_value_of(5));
}

void run_invalid_loop_control_errors() {
  {
    bool failed = false;
    try {
      spark::Interpreter interpreter;
      spark::Parser parser("break\n");
      auto program = parser.parse_program();
      (void)interpreter.run(*program);
    } catch (const spark::EvalException&) {
      failed = true;
    }
    assert(failed);
  }
  {
    bool failed = false;
    try {
      spark::Interpreter interpreter;
      spark::Parser parser("continue\n");
      auto program = parser.parse_program();
      (void)interpreter.run(*program);
    } catch (const spark::EvalException&) {
      failed = true;
    }
    assert(failed);
  }
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
  run_affine_fill_builtin_constructor();
  run_numeric_primitive_builtins();
  run_while_hotloop_numeric_fastpath_smoke();
  run_numeric_constructor_family_smoke();
  run_string_primitive_programs();
  run_bench_tick_family_smoke();
  run_switch_try_and_loop_control();
  run_invalid_loop_control_errors();
  return 0;
}
