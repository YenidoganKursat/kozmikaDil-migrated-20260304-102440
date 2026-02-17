# Phase 8 Research Notes

Phase 8 tasarımında kullanılan ana referanslar:

- Halide (algorithm/schedule ayrımı):
  - [Halide home](https://halide-lang.org/)
  - [Halide paper](https://halide-lang.org/papers/halide12.pdf)
- TVM / Ansor (auto-scheduler yaklaşımı):
  - [TVM OSDI'18](https://www.usenix.org/system/files/osdi18-chen.pdf)
  - [Ansor OSDI'20](https://www.usenix.org/system/files/osdi20-zheng.pdf)
- BLIS (GEMM loop decomposition ve micro-kernel yaklaşımı):
  - [BLIS FAQ](https://github.com/flame/blis/blob/master/docs/FAQ.md)
- BLAS davranışı ve GEMM referansı:
  - [Netlib CBLAS quick reference](https://www.netlib.org/blas/blast-forum/cblas.tgz)
- Matmul sonrası fused post-op prensibi:
  - [oneDNN performance tips](https://uxlfoundation.github.io/oneDNN/dev_guide_performance_settings.html)
- Compiler metadata/passes:
  - [LLVM LangRef](https://llvm.org/docs/LangRef.html)
  - [MLIR Affine dialect](https://mlir.llvm.org/docs/Dialects/Affine/)

## Tasarıma yansıyan kararlar

- Schedule semantiği değiştirmez, sadece performans kararlarını taşır.
- Hybrid backend aktif: BLAS varsa kullanılabilir; yoksa own kernel fallback.
- Pack-cache + probe doğrulaması ile steady-state hız hedeflenir.
- Epilogue fusion (`matmul_add`, `matmul_axpby`) intermediate üretimini azaltır.
- Tuning sonucu JSON ile cachelenir ve runtime’da düşük overhead ile uygulanır.
