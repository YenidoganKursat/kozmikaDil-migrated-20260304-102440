#include <cassert>
#include <cmath>

#include "phase5_support.h"

namespace {

double as_number(const spark::Value& value) {
  if (value.kind == spark::Value::Kind::Int) {
    return static_cast<double>(value.int_value);
  }
  if (value.kind == spark::Value::Kind::Double) {
    return value.double_value;
  }
  if (value.kind == spark::Value::Kind::Numeric) {
    return spark::Interpreter::to_number(value);
  }
  assert(false && "expected numeric value");
  return 0.0;
}

void test_matrix_large_fill_reduce_matches_loop() {
  const char* source = R"(
rows = 48
cols = 40
m = matrix_i64(rows, cols)
r = 0
expected = 0
while r < rows:
  c = 0
  while c < cols:
    v = (r * 13 + c * 7) % 97 - 48
    m[r, c] = v
    expected = expected + v
    c = c + 1
  r = r + 1
sum_reduce = m.reduce_sum()
sum_loop = 0
r = 0
while r < rows:
  c = 0
  while c < cols:
    sum_loop = sum_loop + m[r, c]
    c = c + 1
  r = r + 1
)";
  const auto sum_reduce = phase5_test::run_and_get(source, "sum_reduce");
  const auto sum_loop = phase5_test::run_and_get(source, "sum_loop");
  const auto expected = phase5_test::run_and_get(source, "expected");
  assert(std::fabs(as_number(sum_reduce) - as_number(sum_loop)) < 1e-9);
  assert(std::fabs(as_number(sum_reduce) - as_number(expected)) < 1e-9);
}

void test_matrix_elementwise_chain_preserves_sum() {
  const char* source = R"(
base = matrix_fill_affine(32, 32, 17, 9, 127, 0.5)
lhs = (((base + 5) * 2) - 10) / 2
sum_base = base.reduce_sum()
sum_lhs = lhs.reduce_sum()
)";
  const auto sum_base = phase5_test::run_and_get(source, "sum_base");
  const auto sum_lhs = phase5_test::run_and_get(source, "sum_lhs");
  assert(std::fabs(as_number(sum_base) - as_number(sum_lhs)) < 1e-9);
}

void test_matrix_transpose_roundtrip_and_slices() {
  const char* source = R"(
m = [[1, 2, 3, 4]; [5, 6, 7, 8]; [9, 10, 11, 12]]
tt = m.T.T
probe = tt[2][3]
row = tt[1]
col = tt[:, 2]
block = tt[0:2, 1:3]
)";
  phase5_test::expect_global_int(source, "probe", 12);
  phase5_test::expect_global_list(source, "row", {5, 6, 7, 8});
  phase5_test::expect_global_list(source, "col", {3, 7, 11});
  phase5_test::expect_global_matrix(source, "block", 2, 2, {2, 3, 6, 7});
}

}  // namespace

namespace phase5_test {

void run_matrix_container_extreme_tests() {
  test_matrix_large_fill_reduce_matches_loop();
  test_matrix_elementwise_chain_preserves_sum();
  test_matrix_transpose_roundtrip_and_slices();
}

}  // namespace phase5_test
