// Phase 3 tier classification tests (T4/T5/T8 style labels).
// Bu set, optimize edilebilir bölgelerin otomatik ayrımını doğrular.

#include "phase3/typecheck_support.h"

namespace phase3_test {

void run_tier_classification_tests() {
  expect_tier("def phase3_fn_t4_const():\n  return 1", "phase3_fn_t4_const", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_arith():\n"
      "  x = 1\n"
      "  y = 2\n"
      "  return x + y",
      "phase3_fn_t4_arith", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_if():\n"
      "  x = 0\n"
      "  if x:\n"
      "    x = 1\n"
      "  else:\n"
      "    x = 2\n"
      "  return x",
      "phase3_fn_t4_if", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_while():\n"
      "  i = 0\n"
      "  total = 0\n"
      "  while i < 3:\n"
      "    total = total + 1\n"
      "    i = i + 1\n"
      "  return total",
      "phase3_fn_t4_while", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_for_list():\n"
      "  total = 0\n"
      "  for i in [1, 2, 3]:\n"
      "    total = total + i\n"
      "  return total",
      "phase3_fn_t4_for_list", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_for_range():\n"
      "  total = 0\n"
      "  for i in range(3):\n"
      "    total = total + i\n"
      "  return total",
      "phase3_fn_t4_for_range", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_matrix():\n"
      "  matrix = [[1, 2], [3, 4]]\n"
      "  return matrix[0][1]",
      "phase3_fn_t4_matrix", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_logic():\n"
      "  return not False and True",
      "phase3_fn_t4_logic", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_cmp():\n"
      "  return 1 + 2 == 3",
      "phase3_fn_t4_cmp", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_list_add():\n"
      "  left = [1, 2]\n"
      "  right = [3, 4]\n"
      "  both = left + right\n"
      "  return both",
      "phase3_fn_t4_list_add", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t5_append():\n"
      "  values = []\n"
      "  values.append(1)\n"
      "  return values[0]",
      "phase3_fn_t5_append", spark::TierLevel::T5);
  expect_tier(
      "def phase3_fn_t5_append_widen():\n"
      "  values = []\n"
      "  values.append(1)\n"
      "  values.append(2.5)\n"
      "  return values[1]",
      "phase3_fn_t5_append_widen", spark::TierLevel::T5);
  expect_tier(
      "def phase3_fn_t5_list_mixed():\n"
      "  values = [1, True]\n"
      "  return values[0]",
      "phase3_fn_t5_list_mixed", spark::TierLevel::T5);
  expect_tier(
      "def phase3_fn_t5_matrix_mixed():\n"
      "  m = [[1, 2], [3, True]]\n"
      "  return m[0][0]",
      "phase3_fn_t5_matrix_mixed", spark::TierLevel::T5);
  expect_tier(
      "def phase3_fn_t8_print():\n"
      "  print(1)\n"
      "  return 1",
      "phase3_fn_t8_print", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t8_func_helper():\n"
      "  return 1\n"
      "def phase3_fn_t8_func_caller():\n"
      "  return phase3_fn_t8_func_helper()",
      "phase3_fn_t8_func_helper", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t8_func_helper():\n"
      "  return 1\n"
      "def phase3_fn_t8_func_caller():\n"
      "  return phase3_fn_t8_func_helper()",
      "phase3_fn_t8_func_caller", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t4_not_expr():\n"
      "  return not not False",
      "phase3_fn_t4_not_expr", spark::TierLevel::T4);
  expect_tier(
      "def phase3_fn_t5_append_in_loop():\n"
      "  values = []\n"
      "  for i in [1, 2, 3]:\n"
      "    values.append(i)\n"
      "  return values[1]",
      "phase3_fn_t5_append_in_loop", spark::TierLevel::T5);
  expect_tier(
      "def phase3_fn_t4_matrix_sep():\n"
      "  m = [[1,2];[3,4]]\n"
      "  return m[0][0]",
      "phase3_fn_t4_matrix_sep", spark::TierLevel::T4);
}

}  // namespace phase3_test
