// Phase 3 test entrypoint.
// Her dosya tek bir sorumluluğa odaklanıyor; burada sadece yürütme sırası toplanır.

#include "typecheck_support.h"

int main() {
  semantic_runtime_test::run_core_typecheck_tests();
  semantic_runtime_test::run_tier_classification_tests();
  semantic_runtime_test::run_inference_shape_tests();
  return 0;
}
