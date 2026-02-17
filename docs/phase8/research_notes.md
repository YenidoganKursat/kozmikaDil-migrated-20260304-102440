# Phase 8 Research Notes

Bu fazda matmul tarafı için "algorithm vs schedule" ayrımı, cache-aware blocking ve
micro-kernel pratikleri tekrar gözden geçirildi.

## Derin araştırma kaynakları

- Goto/van de Geijn, high-performance GEMM katmanlama:
  - [Anatomy of High-Performance Matrix Multiplication (DOI)](https://doi.org/10.1145/1356052.1356053)
  - [TOMS preprint mirror](https://docslib.org/doc/973693/anatomy-of-high-performance-matrix-multiplication)
- BLIS loop decomposition + micro-kernel yaklaşımı:
  - [BLIS many-threaded GEMM paper (IPDPS 2014)](https://doi.org/10.1109/IPDPS.2014.110)
- Compiler tarafı (loop-vectorize + unroll kararları):
  - [LLVM Auto-Vectorization](https://llvm.org/docs/Vectorizers.html)
- Phase 7-8 fusion yaklaşımı (ara container azaltma düşüncesi):
  - [Weld paper (CIDR)](http://cidrdb.org/cidr2017/papers/p23-palkar-cidr17.pdf)
- Schedule/auto-tuning tarafı:
  - [Halide paper](https://halide-lang.org/papers/halide12.pdf)
  - [TVM OSDI'18](https://www.usenix.org/system/files/osdi18-chen.pdf)
  - [Ansor OSDI'20](https://www.usenix.org/system/files/osdi20-zheng.pdf)

## Bu kaynaklardan alınan ve uygulanan kurallar

- Own GEMM'de inner-loop branch azaltımı:
  - `b_transposed` ayrımı loop dışına taşındı.
- Mikro-kernel prensibi:
  - `n>=192` için 4-kolon register birikimli yol aktif edildi.
  - `n>=256` için 8-kolon register birikimli yol eklendi.
- Büyük boyutlarda cache/packing maliyeti:
  - `matrix_fill_affine` çıktılarında (>=128^2) dense f64 cache hazırlığı eklendi.
  - BLAS non-transpose giriş yolunda gereksiz repack azaltıldı.
- Auto backend deterministikliği:
  - küçük/orta boyutta own, büyük boyutta BLAS seçimi korunuyor.
  - küçük problemlerde gereksiz BLAS probe çağrısı engellendi.

## Not

Phase 8 kapsamında doğruluk önceliği korundu:
- fast-math default kapalı,
- tüm optimizasyonlar strict semantics altında tutuldu.

## Fast Follow Optimization Log (2026-02-17, row-tile parallel own GEMM)

Denenen yaklaşım:
- `own` GEMM yolunda sadece bağımsız satır-tile bloklarını paralel çalıştıran bir
  worker bölüşümü eklendi.
- CPU feature dispatch tag'ına bağlı kernel eşiği profilleri eklendi
  (`x86_avx2/avx512`, `arm_neon/sve/sve2`, `riscv_rvv`):
  - 8-kolon ve 4-kolon mikro-kernel giriş eşikleri varyanta göre ayarlanıyor.
  - paralel çalışma varsayılan thread cap / hacim eşiği varyanta göre ayarlanıyor.
- Paralel yol sadece yeterli iş hacminde açılıyor:
  - varsayılan minimum hacim: `SPARK_MATMUL_OWN_PAR_MIN_VOLUME`
    (variant tabanlı default, örn. arm_neon için `~3*1024*1024`)
  - worker limiti: `SPARK_MATMUL_OWN_THREADS` (variant tabanlı cap, env ile override)
- Semantik güvenliği için satır-blokları disjoint yazıyor; reduction order değişmeden kaldı.

Ölçüm notları:
- İlk ölçümde 256 auto-vs-selected oranı gürültü nedeniyle bozuldu (`comparisons 3/4`).
- İkinci ölçümde aynı kodla tekrar koşuda tüm karşılaştırmalar geçti (`4/4`).

Son ölçüm snapshot (phase8 benchmark):
- `matmul_core_f64_256`:
  - önceki tipik `c/own ~0.247`
  - yeni `c/own ~0.308` (yaklaşık +%25 göreli iyileşme)
- `matmul_core_f64_512`:
  - önceki tipik `c/own ~0.300`
  - yeni `c/own ~0.411` (yaklaşık +%37 göreli iyileşme)
- `auto/selected` oranı 256 ve 512 için tekrar `~1.0` bandına döndü.

Kontrollü A/B (aynı host, own backend, 9 tekrar medyan):
- `matmul_core_f64_256.k`:
  - `SPARK_MATMUL_OWN_THREADS=1` medyan: `~0.02939s`
  - `SPARK_MATMUL_OWN_THREADS=4` medyan: `~0.02450s`
  - hızlanma: `~1.20x`
- `matmul_core_f64_512.k`:
  - `threads=1` medyan: `~0.07498s`
  - `threads=4` medyan: `~0.05377s`
  - hızlanma: `~1.39x`

Forced-variant smoke (aynı host, own backend):
- `SPARK_CPU_ARCH=aarch64`, `SPARK_CPU_FEATURES=sve2` ile dispatch profili zorlandığında
  (gerçek ISA instruction değil, tuning-profile seçimi):
  - `matmul_core_f64_256.k`: medyan `~0.02308s` vs default `~0.02318s`
  - `matmul_core_f64_512.k`: medyan `~0.04919s` vs default `~0.05142s`

Finalized phase8 fast-follow tuning on this host:
- `arm_neon` dispatch defaults updated to:
  - `j8_threshold=max` (disable 8-col path by default on NEON)
  - `j4_threshold=128`
  - `default_thread_cap=12` (still clamped by `hardware_concurrency`)
  - `min_parallel_volume=3*1024*1024`
- Added explicit runtime tuning overrides:
  - `SPARK_MATMUL_OWN_J8_THRESHOLD`
  - `SPARK_MATMUL_OWN_J4_THRESHOLD`
  - `SPARK_MATMUL_OWN_DEFAULT_THREADS`
  - `SPARK_MATMUL_OWN_DEFAULT_MIN_VOLUME`

Rejected candidate:
- `arm_neon` with `j8_threshold=224` looked good in a few short samples,
  but wider repeated runs showed regressions on `256` cases.
- Kept `j8=max` and improved only thread-cap policy for more stable behavior.
