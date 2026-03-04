#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace spark {

struct CpuFeatureInfo {
  std::string arch;
  std::vector<std::string> features;
};

CpuFeatureInfo detect_cpu_features();
bool cpu_has_feature(std::string_view name);
std::string cpu_feature_report();

// Kernel dispatch helper: best-effort host variant tag.
std::string matmul_variant_tag(bool use_f32);
std::size_t recommended_vector_width(bool use_f32);

}  // namespace spark
