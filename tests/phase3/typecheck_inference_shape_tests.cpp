// Phase 3 inferans ve shape doğrulama testleri.
// Phase 3 inferans ve shape doğrulama testleri.
// Widening, normalize adayları ve class-open/slots ayrımı burada toplanır.

#include <cassert>

#include "phase3/typecheck_support.h"

namespace phase3_test {

void run_inference_shape_tests() {
  expect_symbol_type("x = []\n"
                     "x.append(1)\n"
                     "x.append(2.5)\n",
                     "x", "List[Float(f64)]");
  expect_symbol_type("x = []\n"
                     "x.append(1)\n"
                     "x.append(True)\n",
                     "x", "List[Any]");
  expect_symbol_type("m = [[1, 2], [3.0, 4]]", "m", "Matrix[Float(f64)][2,2]");
  expect_symbol_type("m = [[1, 2], [3, True]]", "m", "Matrix[Any][2,2]");
  expect_symbol_type("x = []", "x", "List[Unknown]");
  expect_symbol_type("a = [[1, 2]; [3, 4]]\nb = a + 2\n", "b", "Matrix[Int][2,2]");
  expect_symbol_type("a = [[1, 2]; [3, 4]]\nb = 2 + a\n", "b", "Matrix[Int][2,2]");
  expect_symbol_type("a = [[1, 2]; [3, 4]]\nb = a + 1.5\n", "b", "Matrix[Float(f64)][2,2]");
  expect_symbol_type("a = [[1, 2]; [3, 4]]\nb = a * a\n", "b", "Matrix[Int][2,2]");
  expect_symbol_type("a = [[1, 2, 3]; [4, 5, 6]]\nb = [[7, 8]; [9, 10]; [11, 12]]\nc = a * b\n",
                     "c", "Matrix[Int][2,2]");
  expect_type_errors("a = [[1, 2]; [3, 4]]\nb = [[1, 2]; [3, 4]; [5, 6]]\nc = a * b\n", 1);
  expect_type_errors("a = [[1, 2]; [3, 4]]\nb = a + [[1,2,3]; [4,5,6]]\n", 1);
  expect_symbol_type("a = [[1, 2]; [3, 4]]\nb = a + True\n", "b", "Matrix[String][2,2]");
  expect_symbol_type("values = [1,2]\nvalue = values.pop()\n", "value", "Int");
  expect_symbol_type("values = [1,2]\nvalues.insert(1, 9)\n", "values", "List[Int]");
  expect_symbol_type("values = [1,2]\nvalue = values.pop(0)\n", "value", "Int");
  expect_symbol_type("values = [1,2]\nvalues.remove(2)\n", "values", "List[Int]");

  spark::Parser parser(R"(
class OpenBox(open):
  x = 1
class SlotBox:
  x = 1
)");
  auto program = parser.parse_program();
  spark::TypeChecker checker;
  checker.check(*program);
  assert(!checker.has_errors());

  const auto* open_shape = find_shape(checker, "OpenBox");
  const auto* slot_shape = find_shape(checker, "SlotBox");
  assert(open_shape != nullptr);
  assert(slot_shape != nullptr);
  assert(open_shape->open);
  assert(!slot_shape->open);
  assert(open_shape->fields.size() == 1);
  assert(slot_shape->fields.size() == 1);
}

}  // namespace phase3_test
