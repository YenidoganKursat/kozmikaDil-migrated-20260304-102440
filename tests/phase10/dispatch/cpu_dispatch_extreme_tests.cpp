#include <cassert>
#include <cstdlib>
#include <string>
#include <utility>

#include "../phase10_support.h"
#include "spark/cpu_features.h"

namespace phase10_test {

namespace {

struct ScopedEnv {
  std::string key;
  std::string old;
  bool had_value = false;

  explicit ScopedEnv(std::string name) : key(std::move(name)) {
    if (const auto* value = std::getenv(key.c_str())) {
      had_value = true;
      old = value;
    }
  }

  void set(const std::string& value) const {
    setenv(key.c_str(), value.c_str(), 1);
  }

  ~ScopedEnv() {
    if (had_value) {
      setenv(key.c_str(), old.c_str(), 1);
    } else {
      unsetenv(key.c_str());
    }
  }
};

void test_unknown_arch_falls_back_to_scalar_dispatch() {
  ScopedEnv arch("SPARK_CPU_ARCH");
  ScopedEnv features("SPARK_CPU_FEATURES");
  arch.set("unknown-simulated");
  features.set("mystery,foo");
  assert(spark::phase8_matmul_variant_tag(false) == "scalar");
  assert(spark::phase8_recommended_vector_width(false) == 2);
  assert(spark::phase8_recommended_vector_width(true) == 4);
}

void test_feature_list_dedup_and_separator_parsing() {
  ScopedEnv arch("SPARK_CPU_ARCH");
  ScopedEnv features("SPARK_CPU_FEATURES");
  arch.set("x86_64");
  features.set("avx2, avx2;avx512f  avx512f");
  assert(spark::phase8_matmul_variant_tag(false) == "x86_avx512");
  assert(spark::phase8_recommended_vector_width(false) == 8);
  assert(spark::phase8_recommended_vector_width(true) == 16);
}

void test_f32_dispatch_width_for_all_supported_arch_overrides() {
  ScopedEnv arch("SPARK_CPU_ARCH");
  ScopedEnv features("SPARK_CPU_FEATURES");

  arch.set("aarch64");
  features.set("neon,sve2");
  assert(spark::phase8_matmul_variant_tag(true) == "arm_sve2");
  assert(spark::phase8_recommended_vector_width(true) == 8);

  arch.set("riscv64");
  features.set("rv64,rvv");
  assert(spark::phase8_matmul_variant_tag(true) == "riscv_rvv");
  assert(spark::phase8_recommended_vector_width(true) == 8);
}

}  // namespace

void run_phase10_dispatch_extreme_tests() {
  test_unknown_arch_falls_back_to_scalar_dispatch();
  test_feature_list_dedup_and_separator_parsing();
  test_f32_dispatch_width_for_all_supported_arch_overrides();
}

}  // namespace phase10_test
