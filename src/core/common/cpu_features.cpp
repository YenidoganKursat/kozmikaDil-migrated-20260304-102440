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
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
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

void detect_arm_features(std::vector<std::string>& out) {
#if defined(__arm__) || defined(__thumb__) || defined(_M_ARM)
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__ARM_FEATURE_NEON)
  add_if(true, "neon", out);
#endif
  add_if(true, "arm32", out);
#endif
}

void detect_riscv_features(std::vector<std::string>& out) {
#if defined(__riscv)
  add_if(true, "riscv", out);
#if defined(__riscv_xlen) && (__riscv_xlen == 32)
  add_if(true, "rv32", out);
#else
  add_if(true, "rv64", out);
#endif
#if defined(__riscv_vector)
  add_if(true, "rvv", out);
#endif
#endif
}

void detect_ppc_features(std::vector<std::string>& out) {
#if defined(__powerpc64__) || defined(_ARCH_PPC64)
#if defined(__GNUC__) || defined(__clang__)
  add_if(__builtin_cpu_supports("altivec") != 0, "altivec", out);
  add_if(__builtin_cpu_supports("vsx") != 0, "vsx", out);
#else
  add_if(true, "ppc", out);
#endif
#elif defined(__powerpc__) || defined(_ARCH_PPC)
  add_if(true, "ppc", out);
#endif
}

void detect_s390_features(std::vector<std::string>& out) {
#if defined(__s390x__) || defined(__s390__) || defined(_ARCH_S390)
  add_if(true, "s390", out);
#endif
}

void detect_loongarch_features(std::vector<std::string>& out) {
#if defined(__loongarch64) || defined(__loongarch64__)
  add_if(true, "loongarch64", out);
#endif
}

void detect_mips_features(std::vector<std::string>& out) {
#if defined(__mips__) || defined(__mips64) || defined(__mips64__)
  add_if(true, "mips", out);
#endif
}

void detect_wasm_features(std::vector<std::string>& out) {
#if defined(__wasm32__) || defined(__wasm64__)
  add_if(true, "wasm", out);
#endif
}

}  // namespace

bool arch_is_riscv(const std::string& arch) {
  return arch.rfind("riscv", 0) == 0;
}

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
#elif defined(__i386__) || defined(_M_IX86)
  info.arch = "x86";
#elif defined(__aarch64__)
  info.arch = "aarch64";
#elif defined(__arm__) || defined(__thumb__) || defined(_M_ARM)
  info.arch = "arm32";
#elif defined(__riscv)
#if defined(__riscv_xlen) && (__riscv_xlen == 32)
  info.arch = "riscv32";
#else
  info.arch = "riscv64";
#endif
#elif defined(__powerpc64__) || defined(_ARCH_PPC64)
#if defined(__LITTLE_ENDIAN__)
  info.arch = "ppc64le";
#else
  info.arch = "ppc64";
#endif
#elif defined(__powerpc__) || defined(_ARCH_PPC)
  info.arch = "ppc";
#elif defined(__s390x__) || defined(_ARCH_S390)
  info.arch = "s390x";
#elif defined(__s390__)
  info.arch = "s390";
#elif defined(__loongarch64) || defined(__loongarch64__)
  info.arch = "loongarch64";
#elif defined(__mips64) || defined(__mips64__)
  info.arch = "mips64";
#elif defined(__mips) || defined(__mips__)
  info.arch = "mips";
#elif defined(__wasm32__)
  info.arch = "wasm32";
#elif defined(__wasm64__)
  info.arch = "wasm64";
#else
  info.arch = "unknown";
#endif

  detect_x86_features(info.features);
  detect_aarch64_features(info.features);
  detect_arm_features(info.features);
  detect_riscv_features(info.features);
  detect_ppc_features(info.features);
  detect_s390_features(info.features);
  detect_loongarch_features(info.features);
  detect_mips_features(info.features);
  detect_wasm_features(info.features);
  return info;
}

bool cpu_has_feature(std::string_view name) {
  const auto info = detect_cpu_features();
  return contains_feature(info.features, name);
}

std::string matmul_variant_tag(bool use_f32) {
  const auto info = detect_cpu_features();
  (void)use_f32;
  if (info.arch == "x86_64" || info.arch == "x86") {
    if (contains_feature(info.features, "avx512f")) {
      return "x86_avx512";
    }
    if (contains_feature(info.features, "avx2")) {
      return "x86_avx2";
    }
    return "x86_baseline";
  }
  if (info.arch == "aarch64" || info.arch == "arm32") {
    if (contains_feature(info.features, "sve2")) {
      return "arm_sve2";
    }
    if (contains_feature(info.features, "sve")) {
      return "arm_sve";
    }
    if (contains_feature(info.features, "neon")) {
      return "arm_neon";
    }
    return "scalar";
  }
  if (arch_is_riscv(info.arch)) {
    if (contains_feature(info.features, "rvv")) {
      return "riscv_rvv";
    }
    return "riscv_baseline";
  }
  if (info.arch == "ppc64" || info.arch == "ppc64le" || info.arch == "ppc" ||
      info.arch == "s390x" || info.arch == "s390" || info.arch == "loongarch64" ||
      info.arch == "mips" || info.arch == "mips64" || info.arch == "wasm32" ||
      info.arch == "wasm64" || info.arch == "armv7") {
    return "scalar";
  }
  return "scalar";
}

std::size_t recommended_vector_width(bool use_f32) {
  const auto tag = matmul_variant_tag(use_f32);
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
  out << "matmul_variant_f64=" << matmul_variant_tag(false) << "\n";
  out << "matmul_variant_f32=" << matmul_variant_tag(true) << "\n";
  out << "matmul_vector_width_f64=" << recommended_vector_width(false) << "\n";
  out << "matmul_vector_width_f32=" << recommended_vector_width(true) << "\n";
  return out.str();
}

}  // namespace spark
