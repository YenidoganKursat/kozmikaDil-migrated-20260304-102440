#include <cassert>
#include <string>

#include "spark/codegen.h"
#include "spark/parser.h"

namespace {

void expect_success(const std::string& source, const std::string& expected_snippet) {
  spark::Parser parser(source);
  auto program = parser.parse_program();

  spark::CodeGenerator generator;
  const auto result = generator.generate(*program);
  assert(result.success);
  assert(!result.output.empty());
  assert(result.output.find(expected_snippet) != std::string::npos);
}

void expect_failure(const std::string& source) {
  spark::Parser parser(source);
  auto program = parser.parse_program();

  spark::CodeGenerator generator;
  const auto result = generator.generate(*program);
  assert(!result.success);
  assert(!result.diagnostics.empty());
}

void expect_irc_success(const std::string& source, const std::string& expected_signature,
                        bool expect_no_percent = false) {
  spark::Parser parser(source);
  auto program = parser.parse_program();

  spark::CodeGenerator generator;
  const auto result = generator.generate(*program);
  assert(result.success);
  spark::IRToCGenerator c_generator;
  const auto c_result = c_generator.translate(result.output);
  assert(c_result.success);
  assert(!c_result.output.empty());
  assert(c_result.output.find(expected_signature) != std::string::npos);
  if (expect_no_percent) {
    assert(c_result.output.find('%') == std::string::npos);
  }
}

void test_codegen_function_and_call() {
  expect_success(
      R"(

def add(a, b):
  return a + b

x = add(1, 2)
)",
      "function @add");
}

void test_codegen_scalar_branch() {
  expect_success(
      R"(

a = 0
if a:
  a = 1
else:
  a = 2
)",
      "function @__main__");
}

void test_codegen_for_loop() {
  expect_success(
      R"(

sum = 0
for i in range(3):
  sum = sum + i
)",
      "cmp.lt.i64");
}

void test_codegen_while_loop() {
  expect_success(
      R"(

x = 0
while x < 2:
  x = x + 1
)",
      "br_if");
}

void test_codegen_list_literal() {
  expect_success(
      R"(

values = [1, 2, 3]
)",
      "__spark_list_append_i64");
}

void test_codegen_list_methods() {
  expect_success(
      R"(

values = [1, 2, 3]
values.append(4)
tail = values.pop()
values.insert(0, 9)
values.remove(2)
)",
      "__spark_list_pop_i64");
}

void test_codegen_matrix_slice_indexing() {
  expect_success(
      R"(

mat = [[1, 2, 3]; [4, 5, 6]; [7, 8, 9]]
col = mat[:, 1]
block = mat[0:2, 1:3]
)",
      "__spark_matrix_slice_block_i64");
}

void test_codegen_matrix_for_loop_rows() {
  expect_success(
      R"(

mat = [[1, 2]; [3, 4]; [5, 6]]
acc = 0
for row in mat:
  acc = acc + row[0]
)",
      "__spark_matrix_row_i64");
}

void test_codegen_list_slice_default_stop() {
  expect_success(
      R"(

values = [1, 2, 3, 4]
tail = values[1:]
)",
      "__spark_list_len_i64");
}

void test_codegen_print_call() {
  expect_success(
      R"(

def scale(v):
  return v

print(scale(3.5))
)",
      "call @print");
}

void test_codegen_emit_c_print_stmt() {
  expect_irc_success(
      R"(

def scale(v):
  return v

print(scale(3.5))
)",
      "__spark_print_f64");
}

void test_codegen_unsupported_indexing() {
  expect_failure(
      R"(

a = 1
b = a[0]
)");
}

void test_codegen_emit_c_translates_temps() {
  expect_irc_success(
      R"(

def add(a, b):
  return a + b

  x = add(1, 2)
)",
      "long long add(long long a, long long b)",
      false);
}

void test_codegen_emit_c_main_entry() {
  expect_irc_success(
      R"(

x = 1
y = x + 2
)",
      "int main()",
      false);
}

void test_codegen_emit_c_append_loop_reserve() {
  expect_irc_success(
      R"(

n = 8
i = 0
values = []
while i < n:
  values.append(i)
  i = i + 1
)",
      "__spark_list_ensure_i64(values, n);",
      false);
}

void test_codegen_emit_c_append_loop_unchecked_fast_path() {
  expect_irc_success(
      R"(

n = 8
i = 0
values = []
while i < n:
  values.append(i)
  i = i + 1
)",
      "__spark_list_append_unchecked_i64(values, i);",
      false);
}

void test_codegen_pow_operator() {
  expect_success(
      R"(

a = 2
b = 8
c = a ^ b
)",
      "pow.f64");
}

void test_codegen_emit_c_pow_runtime() {
  expect_irc_success(
      R"(

a = f64(9.0)
b = f64(0.5)
c = a ^ b
print(c)
)",
      "__spark_num_pow_f64",
      false);
}

void test_codegen_emit_c_numeric_repeat_fast_path() {
  expect_irc_success(
      R"(

i = 0
n = 100
b = f64(3.25)
acc = f64(0)
while i < n:
  acc = acc + b
  i = i + 1
print(acc)
)",
      "__spark_num_repeat_add_f64",
      false);
}

void test_codegen_string_native_path() {
  expect_success(
      R"(

a = "koz"
b = "mika"
c = a + b
n = len(c)
u8 = utf8_len(c)
u16 = utf16_len(c)
ch = c[2]
mid = c[1:4]
print(c)
)",
      "__spark_string_concat");
}

void test_codegen_emit_c_string_runtime() {
  expect_irc_success(
      R"(

s = string "kozmika"
print(s)
)",
      "__spark_string __spark_string_from_utf8(",
      false);
}

void test_codegen_emit_c_string_function_signature() {
  expect_irc_success(
      R"(

def join(a, b):
  return a + b

print(join("a", "b"))
)",
      "static __spark_string join(__spark_string a, __spark_string b)",
      false);
}

}  // namespace

int main() {
  test_codegen_function_and_call();
  test_codegen_scalar_branch();
  test_codegen_for_loop();
  test_codegen_while_loop();
  test_codegen_list_literal();
  test_codegen_list_methods();
  test_codegen_matrix_slice_indexing();
  test_codegen_matrix_for_loop_rows();
  test_codegen_list_slice_default_stop();
  test_codegen_print_call();
  test_codegen_unsupported_indexing();
  test_codegen_emit_c_translates_temps();
  test_codegen_emit_c_print_stmt();
  test_codegen_emit_c_main_entry();
  test_codegen_emit_c_append_loop_reserve();
  test_codegen_emit_c_append_loop_unchecked_fast_path();
  test_codegen_pow_operator();
  test_codegen_emit_c_pow_runtime();
  test_codegen_emit_c_numeric_repeat_fast_path();
  test_codegen_string_native_path();
  test_codegen_emit_c_string_runtime();
  test_codegen_emit_c_string_function_signature();
  return 0;
}
