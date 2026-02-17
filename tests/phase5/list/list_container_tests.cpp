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
  test_list_type_checks();
  test_list_row_style_matrix_fallback();
  test_list_edge_bounds();
}

}  // namespace phase5_test
