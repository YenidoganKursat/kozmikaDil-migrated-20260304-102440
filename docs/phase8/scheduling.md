# Phase 8 Scheduling

## Goal

Phase 8 hedefi matmul çekirdeğinde "algorithm vs schedule" ayrımını netleştirmek:

- Algorithm: `C = A x B`
- Schedule: backend, tile boyutları, unroll, vector width, packing stratejisi

## KernelIR (runtime-side)

`MatmulKernelIR` alanları:

- `m`, `n`, `k`
- `use_f32`
- `use_f64`

Bu bilgi schedule seçimine girer, semantik sonucu değiştirmez.

## Schedule Parameters

`MatmulSchedule` alanları:

- `backend`: `own` veya `blas`
- `tile_m`, `tile_n`, `tile_k`
- `unroll`
- `vector_width`
- `pack_a`, `pack_b`
- `source` (`default`, `tuned_file`, `env_*`, `auto_*`)

Environment override:

- `SPARK_MATMUL_BACKEND=auto|own|blas`
- `SPARK_MATMUL_TILE_M`
- `SPARK_MATMUL_TILE_N`
- `SPARK_MATMUL_TILE_K`
- `SPARK_MATMUL_UNROLL`
- `SPARK_MATMUL_VECTOR_WIDTH`
- `SPARK_MATMUL_USE_TUNED=0|1`
- `SPARK_MATMUL_TUNED_CONFIG=/path/to/matmul_tuned_schedule.json`
- `SPARK_MATMUL_AUTO_DIM_THRESHOLD` (large-problem BLAS eşiği, default `224`)
- `SPARK_MATMUL_AUTO_VOLUME_THRESHOLD` (large volume BLAS eşiği, default `224^3`)
- `SPARK_MATMUL_AUTO_SMALL_BLAS_DIM_THRESHOLD` (small-problem BLAS eşiği, default `112`)

## Backend Policy (Hybrid Path)

- `own`: phase8 tiled kernel (packed/transposed B destekli)
- `blas`: CBLAS `dgemm/sgemm` çağrısı (varsa)
- `auto`: problem boyutu + BLAS varlığına göre seçim
  - large problem (`>= dim/volume threshold`) -> BLAS
  - very small square-ish problem (`<= small BLAS threshold`) -> BLAS
  - diğerleri -> own

BLAS sembol keşfi:

- macOS Accelerate
- OpenBLAS / libblas

## Packing and Cache

- A ve B için packed buffer cache var.
- Cache key: matrix pointer + shape + cache version + transpose/fp mode.
- Ek güvenlik: probe (`first/middle/last`) ile stale cache kontrolü.
- `matmul_stats()` ile cache hit/pack sayaçları izlenir.

## Epilogue Fusion

Phase 8’de desteklenen fused epilogue:

- `matmul_add(rhs, bias)`:
  - `bias` scalar / list / matrix (broadcast destekli)
- `matmul_axpby(rhs, alpha, beta, accum)`:
  - `alpha*(A x B) + beta*accum`

Amaç: matmul sonrası ayrı pass yaratmadan tek plan içinde hesap.

## Correctness Policy

- `fast-math` yok (default strict numerics).
- Numeric olmayan matrix cell matmul path’inde hata üretir.
- Shape mismatch açık diagnostic ile durdurulur.

## Tuning Workflow

Auto-tuner:

```bash
python3 tune/matmul_tuner.py
```

Çıktı:

- `bench/results/matmul_tuned_schedule.json`

Bu dosya varsa runtime schedule çözümünde otomatik uygulanır (`SPARK_MATMUL_USE_TUNED=1`).
