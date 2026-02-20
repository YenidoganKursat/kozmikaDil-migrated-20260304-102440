#include "phase5_support.h"

namespace {

void test_list_literal_and_indexing() {
  const char* source = R"(
values = [10, 20, 30]
first = values[0]
last = values[2]
)";
  phase5_test::expect_global_list(source, "values", {10, 20, 30});
  phase5_test::expect_global_int(source, "first", 10);
  phase5_test::expect_global_int(source, "last", 30);
}

void test_list_append_and_mutation() {
  const char* source = R"(
values = [1, 2]
values.append(3)
values.append(4)
values[1] = 20
)";
  phase5_test::expect_global_list(source, "values", {1, 20, 3, 4});
}

void test_list_pop_and_remove() {
  const char* source = R"(
values = [1, 2, 3, 4]
tail = values.pop()
middle = values.pop(1)
values.remove(3)
)";
  phase5_test::expect_global_list(source, "values", {1});
  phase5_test::expect_global_int(source, "tail", 4);
  phase5_test::expect_global_int(source, "middle", 2);
}

void test_list_insert() {
  const char* source = R"(
values = [1, 3]
values.insert(1, 2)
values.insert(10, 4)
)";
  phase5_test::expect_global_list(source, "values", {1, 2, 3, 4});
}

void test_list_for_loop_sum() {
  const char* source = R"(
values = [1, 2, 3, 4]
sum = 0
for x in values:
  sum = sum + x
)";
  phase5_test::expect_global_int(source, "sum", 10);
}

void test_list_slice_and_assign() {
  const char* source = R"(
values = [1, 2, 3, 4, 5]
head = values[0:3]
tail = values[1:4]
)";
  phase5_test::expect_global_list(source, "head", {1, 2, 3});
  phase5_test::expect_global_list(source, "tail", {2, 3, 4});
}

void test_list_concat() {
  const char* source = R"(
a = [1, 2]
b = [3, 4]
c = a + b
)";
  phase5_test::expect_global_list(source, "c", {1, 2, 3, 4});
}

void test_list_numeric_operator_arithmetic() {
  const char* source = R"(
values = [1, 2, 3.5, 4]
add = values + 1
sub = values - 1
mul = values * 2
div = values / 2
mod = values % 2
pow = values ^ 2
rsub = 10 - values
rdiv = 10 / values
rmod = 10 % values
rpow = 2 ^ values
)";
  phase5_test::expect_global_list_double(source, "add", {2.0, 3.0, 4.5, 5.0});
  phase5_test::expect_global_list_double(source, "sub", {0.0, 1.0, 2.5, 3.0});
  phase5_test::expect_global_list_double(source, "mul", {2.0, 4.0, 7.0, 8.0});
  phase5_test::expect_global_list_double(source, "div", {0.5, 1.0, 1.75, 2.0});
  phase5_test::expect_global_list_double(source, "mod", {1.0, 0.0, 1.5, 0.0});
  phase5_test::expect_global_list_double(source, "pow", {1.0, 4.0, 12.25, 16.0});
  phase5_test::expect_global_list_double(source, "rsub", {9.0, 8.0, 6.5, 6.0});
  phase5_test::expect_global_list_double(source, "rdiv", {10.0, 5.0, 2.857142857142857, 2.5});
  phase5_test::expect_global_list_double(source, "rmod", {0.0, 0.0, 3.0, 2.0});
  phase5_test::expect_global_list_double(source, "rpow", {2.0, 4.0, 11.313708498985, 16.0});
}

void test_list_hetero_string_object_ops() {
  const char* source = R"(
words = ["a", "bc"]
plus = words + "!"
rplus = ">" + words
repeat = words * 2
objects = [len, range]
obj_plus = objects + "_fn"
)";
  phase5_test::expect_global_list_string(source, "plus", {"a!", "bc!"});
  phase5_test::expect_global_list_string(source, "rplus", {">a", ">bc"});
  phase5_test::expect_global_list_string(source, "repeat", {"aa", "bcbc"});
  phase5_test::expect_global_list_string(source, "obj_plus",
                                         {"<builtin len>_fn", "<builtin range>_fn"});
}

void test_list_type_checks() {
  const char* source = R"(
values = [1, 2, 3.0]
)";
  phase5_test::expect_type(source, "values", "List[Float(f64)]");
}

void test_list_row_style_matrix_fallback() {
  const char* source = R"(
values = [[1, 2], [3, 4]]
left = values[0][1]
)";
  phase5_test::expect_global_int(source, "left", 2);
}

void test_list_edge_bounds() {
  const char* source = R"(
values = [1, 2]
out = values[1]
)";
  phase5_test::expect_global_int(source, "out", 2);
}

void test_list_fill_affine_builtin() {
  const char* source = R"(
values = list_fill_affine(6, 3, 1, 7, 0.5)
sum = values.reduce_sum()
)";
  phase5_test::expect_global_list_double(source, "values", {0.5, 2.0, 0.0, 1.5, 3.0, 1.0});
  phase5_test::expect_global_double(source, "sum", 8.0);
}

}  // namespace

namespace phase5_test {

void run_list_container_tests() {
  test_list_literal_and_indexing();
  test_list_append_and_mutation();
  test_list_pop_and_remove();
  test_list_insert();
  test_list_for_loop_sum();
  test_list_slice_and_assign();
  test_list_concat();
  test_list_numeric_operator_arithmetic();
  test_list_hetero_string_object_ops();
  test_list_type_checks();
  test_list_row_style_matrix_fallback();
  test_list_edge_bounds();
  test_list_fill_affine_builtin();
}

}  // namespace phase5_test
