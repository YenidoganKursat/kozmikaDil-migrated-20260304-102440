// Core phase3 typecheck behavior: valid programs and canonical type errors.
// This file intentionally only contains the fundamental diagnostics checks.

#include "phase3/typecheck_support.h"

namespace phase3_test {

void run_core_typecheck_tests() {
  expect_no_type_errors(R"(
x = 1
y = x + 2
if x:
  y = y + 1
matrix = [[1, 2], [3, 4]]
for i in range(3):
  y = y + matrix[i][0]
def inc():
  return x + 1
z = inc()
)");

  expect_type_errors(R"(
x = 1 + False
)",
                    1);
  expect_type_errors(R"(
x = 1 + y
)",
                    1);
  expect_type_errors(R"(
x = 0
for v in 3:
  x = x + 1
)",
                    1);
  expect_type_errors(R"(
def add(a, b):
  return a + b
x = add(1)
)",
                    1);
  expect_type_errors(R"(
x = range(True, 3, 4, 5)
)",
                    1);
  expect_type_errors(R"(
x = 1
x(1)
)",
                    1);
}

}  // namespace phase3_test
