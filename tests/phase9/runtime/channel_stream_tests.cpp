#include <cassert>
#include <cmath>

#include "../phase9_support.h"

namespace phase9_test {

namespace {

void assert_close(double lhs, double rhs, double tol = 1e-9) {
  assert(std::fabs(lhs - rhs) <= tol);
}

void test_bounded_channel_send_recv_close() {
  constexpr auto source = R"(
def producer(ch):
  send(ch, 1)
  send(ch, 2)
  close(ch)

ch = channel(1)
t = spawn(producer, ch)
a = recv(ch)
b = recv(ch)
c = recv(ch)
_ = join(t)
sum = a + b
drained = c == None
)";
  const auto sum = run_and_get(source, "sum");
  const auto drained = run_and_get(source, "drained");
  assert_close(as_number(sum), 3.0);
  assert(drained.kind == spark::Value::Kind::Bool);
  assert(drained.bool_value);
}

void test_stream_and_anext() {
  constexpr auto source = R"(
def producer(ch):
  send(ch, 5)
  send(ch, 6)
  close(ch)

ch = channel(2)
_ = spawn(producer, ch)
s = stream(ch)
x = anext(s)
y = s.anext()
_z = s.anext()
has_more = s.has_next()
sum = x + y
)";
  const auto sum = run_and_get(source, "sum");
  const auto has_more = run_and_get(source, "has_more");
  assert_close(as_number(sum), 11.0);
  assert(has_more.kind == spark::Value::Kind::Bool);
  assert(!has_more.bool_value);
}

void test_async_for_over_channel_stream() {
  constexpr auto source = R"(
def producer(ch):
  send(ch, 2)
  send(ch, 3)
  send(ch, 5)
  close(ch)

ch = channel(4)
_ = spawn(producer, ch)
acc = 0
async for v in stream(ch):
  acc = acc + v
)";
  const auto acc = run_and_get(source, "acc");
  assert_close(as_number(acc), 10.0);
}

}  // namespace

void run_phase9_channel_stream_tests() {
  test_bounded_channel_send_recv_close();
  test_stream_and_anext();
  test_async_for_over_channel_stream();
}

}  // namespace phase9_test
