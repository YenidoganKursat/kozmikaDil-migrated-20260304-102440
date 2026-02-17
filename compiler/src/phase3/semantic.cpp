// Semantic modülerizasyonu:
// 01_core: tip yardımcıları + temel TypeChecker giriş noktaları
// 02_types_and_names: tip kurucuları + sembol tabanı
// 03_context_tier: bağlam, tier hesapları ve tip uyumluluk kontrolleri
// 04_analysis: expr/stmt analizi
// 05_expr_infer: expression type inference
// 06_dumps: type/shape/tier string çıktıları

#include "spark/semantic.h"

#include "semantic_parts/01_core.cpp"
#include "semantic_parts/02_types_and_names.cpp"
#include "semantic_parts/03_context_tier.cpp"
#include "semantic_parts/04_analysis.cpp"
#include "semantic_parts/05_expr_infer.cpp"
#include "semantic_parts/06_dumps.cpp"

