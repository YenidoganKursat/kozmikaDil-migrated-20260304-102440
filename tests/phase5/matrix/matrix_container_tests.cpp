#include "phase5_support.h"

namespace {

void test_matrix_literal_shape_and_sum() {
  const char* source = R"(
mat = [[1, 2]; [3, 4]; [5, 6]]
diag = mat[0][0] + mat[1][1] + mat[2][1]
rows = 3
cols = 2
)";
  phase5_test::expect_global_matrix(source, "mat", 3, 2, {1, 2, 3, 4, 5, 6});
  phase5_test::expect_global_int(source, "diag", 11);
  phase5_test::expect_global_int(source, "rows", 3);
  phase5_test::expect_global_int(source, "cols", 2);
}

void test_matrix_row_and_column_slice() {
  const char* source = R"(
mat = [[1, 2, 3]; [4, 5, 6]]
first_row = mat[0]
col_values = mat[:, 1]
)";
  phase5_test::expect_global_list(source, "first_row", {1, 2, 3});
  phase5_test::expect_global_list(source, "col_values", {2, 5});
}

void test_matrix_block_slice() {
  const char* source = R"(
mat = [[1, 2, 3]; [4, 5, 6]; [7, 8, 9]]
block = mat[0:2, 1:3]
)";
  phase5_test::expect_global_matrix(source, "block", 2, 2, {2, 3, 5, 6});
}

void test_matrix_transpose() {
  const char* source = R"(
mat = [[1, 2, 3]; [4, 5, 6]]
t = mat.T
first_col = t[0]
last = t[1][1]
)";
  phase5_test::expect_global_matrix(source, "t", 3, 2, {1, 4, 2, 5, 3, 6});
  phase5_test::expect_global_list(source, "first_col", {1, 4});
  phase5_test::expect_global_int(source, "last", 5);
}

void test_matrix_for_loop_rows() {
  const char* source = R"(
mat = [[1, 2]; [3, 4]; [5, 6]]
acc = 0
for row in mat:
  acc = acc + row[0]
)";
  phase5_test::expect_global_int(source, "acc", 9);
}

void test_matrix_type_infers() {
  const char* source = R"(
mat = [[1, 2]; [3, 4]]
)";
  phase5_test::expect_type(source, "mat", "Matrix[Int][2,2]");
}

void test_matrix_elementwise_arithmetic() {
  const char* source = R"(
left = [[1, 2]; [3, 4]]
right = [[1.0, 1.0]; [1.0, 1.0]]
add = left + right
sub = left - 2
mul = left * 2
div = right / left
pow = left ^ 2
rpow = 2 ^ left
)";
  phase5_test::expect_global_matrix_double(source, "add", 2, 2, {2.0, 3.0, 4.0, 5.0});
  phase5_test::expect_global_matrix(source, "sub", 2, 2, { -1, 0, 1, 2});
  phase5_test::expect_global_matrix(source, "mul", 2, 2, {2, 4, 6, 8});
  phase5_test::expect_global_matrix_double(source, "div", 2, 2, {1.0, 0.5, 0.333333333333, 0.25});
  phase5_test::expect_global_matrix_double(source, "pow", 2, 2, {1.0, 4.0, 9.0, 16.0});
  phase5_test::expect_global_matrix_double(source, "rpow", 2, 2, {2.0, 4.0, 8.0, 16.0});
}

void test_matrix_mod_arithmetic() {
  const char* source = R"(
left = [[5, 6]; [7, 8]]
right = [[2, 4]; [3, 5]]
mod_elem = left % right
mod_scalar = left % 3
rmod_scalar = 20 % left
)";
  phase5_test::expect_global_matrix_double(source, "mod_elem", 2, 2, {1.0, 2.0, 1.0, 3.0});
  phase5_test::expect_global_matrix_double(source, "mod_scalar", 2, 2, {2.0, 0.0, 1.0, 2.0});
  phase5_test::expect_global_matrix_double(source, "rmod_scalar", 2, 2, {0.0, 2.0, 6.0, 4.0});
}

void test_matrix_hetero_string_object_ops() {
  const char* source = R"(
left = [["a", "bc"]; ["d", "e"]]
plus = left + "!"
rplus = ">" + left
repeat = left * 2
obj = [[len, range]; [print, string]]
obj_plus = obj + "_fn"
)";
  phase5_test::expect_global_matrix_string(source, "plus", 2, 2, {"a!", "bc!", "d!", "e!"});
  phase5_test::expect_global_matrix_string(source, "rplus", 2, 2, {">a", ">bc", ">d", ">e"});
  phase5_test::expect_global_matrix_string(source, "repeat", 2, 2, {"aa", "bcbc", "dd", "ee"});
  phase5_test::expect_global_matrix_string(source, "obj_plus", 2, 2,
                                           {"<builtin len>_fn", "<builtin range>_fn",
                                            "<builtin print>_fn", "<builtin string>_fn"});
}

void test_matrix_star_is_matmul() {
  const char* source = R"(
left = [[1, 2, 3]; [4, 5, 6]]
right = [[7, 8]; [9, 10]; [11, 12]]
out = left * right
)";
  phase5_test::expect_global_matrix(source, "out", 2, 2, {58, 64, 139, 154});
}

void test_matrix_mutate_by_slice() {
  const char* source = R"(
mat = [[1, 2, 3]; [4, 5, 6]]
row = mat[1]
col = mat[:, 0]
)";
  phase5_test::expect_global_list(source, "row", {4, 5, 6});
  phase5_test::expect_global_list(source, "col", {1, 4});
}

void test_matrix_mutation_assign() {
  const char* source = R"(
mat = [[1, 2]; [3, 4]]
mat[1][1] = 40
mat[0] = [7, 8]
)";
  phase5_test::expect_global_matrix(source, "mat", 2, 2, {7, 8, 3, 40});
}

}  // namespace

namespace phase5_test {

void run_matrix_container_tests() {
  test_matrix_literal_shape_and_sum();
  test_matrix_row_and_column_slice();
  test_matrix_block_slice();
  test_matrix_elementwise_arithmetic();
  test_matrix_mod_arithmetic();
  test_matrix_hetero_string_object_ops();
  test_matrix_star_is_matmul();
  test_matrix_mutate_by_slice();
  test_matrix_transpose();
  test_matrix_for_loop_rows();
  test_matrix_type_infers();
  test_matrix_mutation_assign();
}

}  // namespace phase5_test
