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

void test_large_list_reduce_matches_loop_sum() {
  const char* source = R"(
values = list_fill_affine(2048, 37, 11, 257, 1.0)
sum_reduce = values.reduce_sum()
sum_loop = 0
for v in values:
  sum_loop = sum_loop + v
size = len(values)
)";
  const auto sum_reduce = phase5_test::run_and_get(source, "sum_reduce");
  const auto sum_loop = phase5_test::run_and_get(source, "sum_loop");
  const auto size = phase5_test::run_and_get(source, "size");

  assert(size.kind == spark::Value::Kind::Int);
  assert(size.int_value == 2048);
  assert(std::fabs(as_number(sum_reduce) - as_number(sum_loop)) < 1e-9);
}

void test_list_mutation_stress_integrity() {
  const char* source = R"(
values = []
i = 0
while i < 512:
  values.append(i)
  i = i + 1
i = 0
while i < 128:
  values.pop(0)
  i = i + 1
i = 0
while i < 128:
  values.pop()
  i = i + 1
values.insert(0, -1)
values.insert(len(values), 999)
values[10] = 4242
size = len(values)
head = values[0]
tail = values[len(values) - 1]
probe = values[10]
sum_loop = 0
for v in values:
  sum_loop = sum_loop + v
)";
  phase5_test::expect_global_int(source, "size", 258);
  phase5_test::expect_global_int(source, "head", -1);
  phase5_test::expect_global_int(source, "tail", 999);
  phase5_test::expect_global_int(source, "probe", 4242);
  phase5_test::expect_global_int(source, "sum_loop", 70511);
}

void test_list_operator_chain_preserves_sum() {
  const char* source = R"(
base = list_fill_affine(1024, 19, 5, 101, 0.5)
lhs = (((base + 3) * 4) - 12) / 4
sum_base = base.reduce_sum()
sum_lhs = lhs.reduce_sum()
)";
  const auto sum_base = phase5_test::run_and_get(source, "sum_base");
  const auto sum_lhs = phase5_test::run_and_get(source, "sum_lhs");
  assert(std::fabs(as_number(sum_base) - as_number(sum_lhs)) < 1e-9);
}

}  // namespace

namespace phase5_test {

void run_list_container_extreme_tests() {
  test_large_list_reduce_matches_loop_sum();
  test_list_mutation_stress_integrity();
  test_list_operator_chain_preserves_sum();
}

}  // namespace phase5_test
