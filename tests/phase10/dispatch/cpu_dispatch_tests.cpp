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

  void clear() const {
    unsetenv(key.c_str());
  }

  ~ScopedEnv() {
    if (had_value) {
      setenv(key.c_str(), old.c_str(), 1);
      return;
    }
    unsetenv(key.c_str());
  }
};

void test_cpu_report_basic_shape() {
  const auto report = spark::cpu_feature_report();
  assert(report.find("arch=") != std::string::npos);
  assert(report.find("features=") != std::string::npos);
  assert(report.find("phase8_variant_f64=") != std::string::npos);
  assert(report.find("phase8_vector_width_f64=") != std::string::npos);
}

void test_x86_dispatch_override() {
  ScopedEnv arch("SPARK_CPU_ARCH");
  ScopedEnv features("SPARK_CPU_FEATURES");

  arch.set("x86_64");
  features.set("sse2,avx2");
  assert(spark::phase8_matmul_variant_tag(false) == "x86_avx2");
  assert(spark::phase8_recommended_vector_width(false) == 4);

  features.set("sse2,avx2,avx512f");
  assert(spark::phase8_matmul_variant_tag(false) == "x86_avx512");
  assert(spark::phase8_recommended_vector_width(false) == 8);
}

void test_arm_dispatch_override() {
  ScopedEnv arch("SPARK_CPU_ARCH");
  ScopedEnv features("SPARK_CPU_FEATURES");

  arch.set("aarch64");
  features.set("neon");
  assert(spark::phase8_matmul_variant_tag(false) == "arm_neon");
  assert(spark::phase8_recommended_vector_width(false) == 2);

  features.set("neon,sve2");
  assert(spark::phase8_matmul_variant_tag(false) == "arm_sve2");
  assert(spark::phase8_recommended_vector_width(false) == 4);
}

void test_riscv_dispatch_override() {
  ScopedEnv arch("SPARK_CPU_ARCH");
  ScopedEnv features("SPARK_CPU_FEATURES");

  arch.set("riscv64");
  features.set("rv64,rvv");
  assert(spark::phase8_matmul_variant_tag(false) == "riscv_rvv");
  assert(spark::phase8_recommended_vector_width(false) == 4);

  features.set("rv64");
  assert(spark::phase8_matmul_variant_tag(false) == "riscv_baseline");
}

}  // namespace

void run_phase10_dispatch_tests() {
  test_cpu_report_basic_shape();
  test_x86_dispatch_override();
  test_arm_dispatch_override();
  test_riscv_dispatch_override();
}

}  // namespace phase10_test
