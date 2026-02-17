// Phase 3 test entrypoint.
// Her dosya tek bir sorumluluğa odaklanıyor; burada sadece yürütme sırası toplanır.

#include "phase3/typecheck_support.h"

int main() {
  phase3_test::run_core_typecheck_tests();
  phase3_test::run_tier_classification_tests();
  phase3_test::run_inference_shape_tests();
  return 0;
}
