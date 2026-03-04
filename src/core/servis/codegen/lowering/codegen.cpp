// Codegen modülerizasyonu:
// 01_helpers: temel yardımcı fonksiyonlar, normalize ve parser için ortak dönüşümler
// 02_ir_kind: container/kind mapping ve runtime yardımcıları
// 03_ir_to_c: IRToCGenerator::translate akışı
// 04_codegen_front: generate/infer/compile_main ve blok/satır akışı
// 05_codegen_flow: stmt-emitter ve kontrol akışları
// 06_codegen_expr: expression emitter, tip ve scope yardımcıları

#include "codegen_parts/01_helpers.cpp"
#include "codegen_parts/02_ir_kind.cpp"
#include "codegen_parts/03_ir_to_c.cpp"
#include "codegen_parts/04_codegen_front.cpp"
#include "codegen_parts/dispatch/01_stmt_dispatch.cpp"
#include "codegen_parts/05_codegen_flow.cpp"
#include "codegen_parts/06_codegen_expr.cpp"
