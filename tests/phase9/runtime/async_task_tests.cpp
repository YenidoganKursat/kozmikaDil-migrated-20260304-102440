#include <cassert>
#include <cmath>
#include <string>

#include "../phase9_support.h"

namespace phase9_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_async_fn_and_await() {
  constexpr auto source = R"(
async fn add1(x):
  return x + 1

t = add1(41)
out = await t
)";
  const auto out = run_and_get(source, "out");
  assert_close(as_number(out), 42.0);
}

void test_task_group_structured_join() {
  constexpr auto source = R"(
def sq(x):
  return x * x

with task_group(1000) as g:
  t1 = g.spawn(sq, 3)
  t2 = g.spawn(sq, 4)

vals = g.join_all()
out = vals[0] + vals[1]
)";
  const auto out = run_and_get(source, "out");
  assert_close(as_number(out), 25.0);
}

void test_scheduler_stats_present() {
  constexpr auto source = R"(
def inc(x):
  return x + 1

t = spawn(inc, 9)
_ = join(t)
s = scheduler_stats()
threads = s[0]
spawned = s[1]
executed = s[2]
ok = threads > 0 and spawned >= 1 and executed >= 1
)";
  const auto ok = run_and_get(source, "ok");
  assert(ok.kind == spark::Value::Kind::Bool);
  assert(ok.bool_value);
}

void test_deadline_alias_with_join() {
  constexpr auto source = R"(
async fn id(x):
  return x

t = id(7)
out = join(t, deadline(100))
)";
  const auto out = run_and_get(source, "out");
  assert_close(as_number(out), 7.0);
}

void test_recv_timeout_triggers_error() {
  constexpr auto source = R"(
ch = channel(1)
x = recv(ch, 0)
)";

  bool threw_timeout = false;
  try {
    (void)run_and_get(source, "x");
  } catch (const spark::EvalException& err) {
    const std::string msg = err.what();
    threw_timeout = (msg.find("recv() timeout") != std::string::npos);
  }
  assert(threw_timeout);
}

}  // namespace

void run_phase9_async_task_tests() {
  test_async_fn_and_await();
  test_task_group_structured_join();
  test_scheduler_stats_present();
  test_deadline_alias_with_join();
  test_recv_timeout_triggers_error();
}

}  // namespace phase9_test
