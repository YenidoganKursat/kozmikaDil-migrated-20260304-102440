# Phase 7 Research Notes

Bu notlar Phase 7 tasarım kararları için kullanılan modern kaynakların kısa özetidir.

## Ana kaynak yönleri

- Weld (CIDR) yaklaşımı:
  - veri hareketini azaltmak ve operator fusion ile tek pass çalışmak.
- DataFusion optimizer:
  - projection/filter pushdown ve materialization azaltma mantığı.
- Polars lazy optimizer:
  - query plan seviyesinde fusion ve gereksiz ara tablo üretmeme.
- Julia / Rust performans tartışmaları:
  - iterator/fusion zincirlerinde accidental allocation ve order-semantics riskleri.
- GHC rewrite-rules / stream fusion:
  - “deforest only when semantically safe” prensibi.
- MLIR canonicalization + CSE:
  - sade IR ve deterministik pass sıralamasının önemini destekler.

## Phase 7’ye uygulanan somut sonuçlar

- Fusion sadece güvenli koşullarda aktif.
- Materialization sebepleri raporlanıyor (`--why-not-fused`).
- Heterojen path için plan + cache mantığı korunuyor (Phase 6 uyumlu).
- Fused/non-fused differential benchmark zorunlu hale getirildi.

## Kaynak bağlantıları

- [Weld paper](https://cs.stanford.edu/~matei/papers/2017/cidr_weld.pdf)
- [Apache DataFusion optimizer docs](https://datafusion.apache.org/user-guide/sql/optimizer.html)
- [Polars lazy optimizations](https://docs.pola.rs/user-guide/lazy/optimizations/)
- [Julia performance tips](https://docs.julialang.org/en/v1/manual/performance-tips/)
- [Rust iterators vs loops docs](https://doc.rust-lang.org/book/ch13-04-performance.html)
- [GHC rewrite rules](https://downloads.haskell.org/~ghc/latest/docs/users_guide/exts/rewrite_rules.html)
- [MLIR canonicalization](https://mlir.llvm.org/docs/Canonicalization/)
