#include "spark/cpu_features.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#if defined(__linux__)
#include <sys/auxv.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif
#endif

namespace spark {

namespace {

bool contains_feature(const std::vector<std::string>& features, std::string_view name) {
  return std::find(features.begin(), features.end(), name) != features.end();
}

std::vector<std::string> parse_feature_list(const char* raw) {
  std::vector<std::string> out;
  if (!raw || *raw == '\0') {
    return out;
  }
  std::string token;
  const std::string text(raw);
  for (char ch : text) {
    if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t' || ch == '\n') {
      if (!token.empty()) {
        out.push_back(token);
        token.clear();
      }
      continue;
    }
    token.push_back(ch);
  }
  if (!token.empty()) {
    out.push_back(token);
  }
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void add_if(bool condition, const char* name, std::vector<std::string>& out) {
  if (condition) {
    out.emplace_back(name);
  }
}

void detect_x86_features(std::vector<std::string>& out) {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  add_if(__builtin_cpu_supports("sse2"), "sse2", out);
  add_if(__builtin_cpu_supports("sse4.2"), "sse4.2", out);
  add_if(__builtin_cpu_supports("avx"), "avx", out);
  add_if(__builtin_cpu_supports("fma"), "fma", out);
  add_if(__builtin_cpu_supports("avx2"), "avx2", out);
  add_if(__builtin_cpu_supports("avx512f"), "avx512f", out);
  add_if(__builtin_cpu_supports("avx512bw"), "avx512bw", out);
  add_if(__builtin_cpu_supports("bmi2"), "bmi2", out);
#else
  out.emplace_back("sse2");
#endif
#endif
}

void detect_aarch64_features(std::vector<std::string>& out) {
#if defined(__aarch64__)
  add_if(true, "neon", out);
#if defined(__linux__)
  const auto hwcap = getauxval(AT_HWCAP);
#ifdef HWCAP_ASIMD
  add_if((hwcap & HWCAP_ASIMD) != 0, "asimd", out);
#endif
#ifdef HWCAP_SVE
  add_if((hwcap & HWCAP_SVE) != 0, "sve", out);
#endif
#ifdef HWCAP2_SVE2
  const auto hwcap2 = getauxval(AT_HWCAP2);
  add_if((hwcap2 & HWCAP2_SVE2) != 0, "sve2", out);
#endif
#endif
#endif
}

void detect_riscv_features(std::vector<std::string>& out) {
#if defined(__riscv)
  add_if(true, "rv64", out);
#if defined(__riscv_vector)
  add_if(true, "rvv", out);
#endif
#endif
}

}  // namespace

CpuFeatureInfo detect_cpu_features() {
  CpuFeatureInfo info;
  const auto* forced_arch = std::getenv("SPARK_CPU_ARCH");
  const auto forced_features = parse_feature_list(std::getenv("SPARK_CPU_FEATURES"));
  if ((forced_arch && *forced_arch != '\0') || !forced_features.empty()) {
    info.arch = (forced_arch && *forced_arch != '\0') ? std::string(forced_arch) : "unknown";
    info.features = forced_features;
    return info;
  }

#if defined(__x86_64__) || defined(_M_X64)
  info.arch = "x86_64";
#elif defined(__aarch64__)
  info.arch = "aarch64";
#elif defined(__riscv)
  info.arch = "riscv64";
#else
  info.arch = "unknown";
#endif

  detect_x86_features(info.features);
  detect_aarch64_features(info.features);
  detect_riscv_features(info.features);
  return info;
}

bool cpu_has_feature(std::string_view name) {
  const auto info = detect_cpu_features();
  return contains_feature(info.features, name);
}

std::string phase8_matmul_variant_tag(bool use_f32) {
  const auto info = detect_cpu_features();
  (void)use_f32;
  if (info.arch == "x86_64") {
    if (contains_feature(info.features, "avx512f")) {
      return "x86_avx512";
    }
    if (contains_feature(info.features, "avx2")) {
      return "x86_avx2";
    }
    return "x86_baseline";
  }
  if (info.arch == "aarch64") {
    if (contains_feature(info.features, "sve2")) {
      return "arm_sve2";
    }
    if (contains_feature(info.features, "sve")) {
      return "arm_sve";
    }
    return "arm_neon";
  }
  if (info.arch == "riscv64") {
    if (contains_feature(info.features, "rvv")) {
      return "riscv_rvv";
    }
    return "riscv_baseline";
  }
  return "scalar";
}

std::size_t phase8_recommended_vector_width(bool use_f32) {
  const auto tag = phase8_matmul_variant_tag(use_f32);
  if (tag == "x86_avx512") {
    return use_f32 ? 16 : 8;
  }
  if (tag == "x86_avx2") {
    return use_f32 ? 8 : 4;
  }
  if (tag == "arm_sve2" || tag == "arm_sve") {
    return use_f32 ? 8 : 4;
  }
  if (tag == "arm_neon") {
    return use_f32 ? 4 : 2;
  }
  if (tag == "riscv_rvv") {
    return use_f32 ? 8 : 4;
  }
  return use_f32 ? 4 : 2;
}

std::string cpu_feature_report() {
  const auto info = detect_cpu_features();
  std::ostringstream out;
  out << "arch=" << info.arch << "\n";
  out << "features=";
  for (std::size_t i = 0; i < info.features.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << info.features[i];
  }
  out << "\n";
  out << "phase8_variant_f64=" << phase8_matmul_variant_tag(false) << "\n";
  out << "phase8_variant_f32=" << phase8_matmul_variant_tag(true) << "\n";
  out << "phase8_vector_width_f64=" << phase8_recommended_vector_width(false) << "\n";
  out << "phase8_vector_width_f32=" << phase8_recommended_vector_width(true) << "\n";
  return out.str();
}

}  // namespace spark
