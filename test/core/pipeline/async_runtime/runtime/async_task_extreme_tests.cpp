#include <cassert>
#include <cmath>

#include "../async_runtime_support.h"

namespace async_runtime_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_task_group_many_spawn_join() {
  constexpr auto source = R"(
def inc(x):
  return x + 1

with task_group(2000) as g:
  i = 0
  while i < 64:
    _ = g.spawn(inc, i)
    i = i + 1

vals = g.join_all()
sum = 0
for v in vals:
  sum = sum + v
expected = (64 * 65) / 2
)";
  const auto sum = run_and_get(source, "sum");
  const auto expected = run_and_get(source, "expected");
  assert_close(as_number(sum), as_number(expected));
}

void test_nested_async_await_chain() {
  constexpr auto source = R"(
async fn add1(x):
  return x + 1

async fn add2(x):
  t = add1(x)
  y = await t
  return y + 1

t = add2(40)
out = await t
)";
  const auto out = run_and_get(source, "out");
  assert_close(as_number(out), 42.0);
}

}  // namespace

void run_async_runtime_async_task_extreme_tests() {
  test_task_group_many_spawn_join();
  test_nested_async_await_chain();
}

}  // namespace async_runtime_test
