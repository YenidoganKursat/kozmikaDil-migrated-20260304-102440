# Phase 8 Report

## Scope

Phase 8 hedefi: matmul çekirdeğini schedule/pack/cache altyapısıyla çalıştırmak ve epilogue fusion eklemek.

## Implemented Deliverables

### 1) Kernel + Schedule runtime

Yeni modüller:

- `compiler/src/phase8/runtime/01_schedule.cpp`
- `compiler/src/phase8/runtime/02_pack_cache.cpp`
- `compiler/src/phase8/runtime/03_matmul_kernel.cpp`
- `compiler/src/phase8/runtime/04_matmul_entry.cpp`

Aktif özellikler:

- `MatmulKernelIR` + `MatmulSchedule` çözümü
- backend seçimi: `own` / `blas` / `auto`
- tile/unroll/vector env override
- tuned JSON schedule yükleme (cache'li, çağrı-başı dosya parse yok)
- auto backend eşik kontrolü (`SPARK_MATMUL_AUTO_DIM_THRESHOLD`, `SPARK_MATMUL_AUTO_VOLUME_THRESHOLD`)

### 2) Matmul API (language surface)

Çalışan method’lar:

- `matmul(rhs)`
- `matmul_f32(rhs)`
- `matmul_f64(rhs)`
- `matmul_add(rhs, bias)`
- `matmul_axpby(rhs, alpha, beta, accum)`
- `matmul_stats()`
- `matmul_schedule()`

Ek Phase 8 utility builtin’leri:

- `matrix_fill_affine(...)`
- `matmul_expected_sum(lhs, rhs)`

### 3) Pack/cache + steady-state counters

- A/B packed buffer cache
- cache hit / pack count sayaçları
- `matmul_stats()` ile gözlemlenebilirlik

### 4) Epilogue fusion

- `A x B + bias`
- `alpha*(A x B) + beta*accum`

Bu path’ler ek materialization pass üretmeden çalışır.

### 5) Tuning

- `tune/matmul_tuner.py`
- çıktı: `bench/results/matmul_tuned_schedule.json`

## Validation

### Tests

Yeni test target:

- `sparkc_phase8_tests`

Dosyalar:

- `tests/phase8/matmul/matmul_tests.cpp`
- `tests/phase8/analyze/matmul_analyze_tests.cpp`

Kontrol edilen başlıklar:

- matmul correctness (`f32/f64`)
- epilogue fusion correctness
- cache hit davranışı
- analyze/type dump matmul görünürlüğü

### Benchmarks

Script:

- `bench/scripts/run_phase8_benchmarks.py`

Programs:

- `bench/programs/phase8/matmul_core_f64.k`
- `bench/programs/phase8/matmul_epilogue_f64.k`
- `bench/programs/phase8/matmul_core_f64_256.k`
- `bench/programs/phase8/matmul_core_f64_512.k`

Results:

- `bench/results/phase8_benchmarks.json`
- `bench/results/phase8_benchmarks.csv`
- latest run gate: `records 16/16 PASS`, `comparisons 4/4 PASS`

Latest snapshot:

- `matmul_core_f64`:
  - auto backend: `own`
  - own vs c: `0.213x` (c/own)
  - own vs blas: `0.927x` (own/blas)
  - auto vs selected-backend: `1.013x`
- `matmul_epilogue_f64`:
  - auto backend: `own`
  - own vs c: `0.185x` (c/own)
  - own vs blas: `0.903x` (own/blas)
  - auto vs selected-backend: `1.015x`
- `matmul_core_f64_256`:
  - auto backend: `blas`
  - own vs c: `0.247x` (c/own)
  - own vs blas: `1.160x` (own/blas)
  - auto vs selected-backend: `1.004x`
- `matmul_core_f64_512`:
  - auto backend: `blas`
  - own vs c: `0.300x` (c/own)
  - own vs blas: `1.289x` (own/blas)
  - auto vs selected-backend: `0.972x`

Not:

- Utility builtin’lerle (`matrix_fill_affine`, `matmul_expected_sum`) benchmark orchestration maliyeti düşürüldü.
- Phase8 utility builtins sonrası kernel-dışı setup yükü ciddi biçimde azaldı; ölçüm artık daha çok matmul path'ini temsil ediyor.
- Kararlılık için benchmark scripti `sample_repeat=3` ile çalıştırılıyor ve `auto` kıyası “selected backend”e göre yapılıyor.
- own matmul kernel iç döngüsünde `b_transposed` branch'i loop dışına taşındı.
- epilogue path’inde bias/accumulator shape-check bir kez yapılıp hücre-başı erişim sadeleştirildi.
- `512` benchmarkında toplam checksum büyüdüğü için PASS toleransı (`diff < 1e-4`) ile doğrulanıyor.
- own kernel için `n>=192` boyutlarında 4-kolon register-mikro-kernel yolu etkin.
- own kernel için `n>=256` boyutlarında 8-kolon register-mikro-kernel yolu etkin.
- schedule çözümünde boyuta göre tile adaptasyonu aktif:
  - `<=160` için `96x96x96`
  - `>=224` için `64x64x64`
- auto backend küçük problemlerde gereksiz BLAS probe yapmadan karar veriyor (`large_problem && has_blas_backend()`).
- `matrix_fill_affine` çıktısında (>=`128^2`) dense f64 cache hazırlandığı için BLAS non-transpose giriş yolu daha düşük overhead ile çalışıyor.

## Exit Criteria Check (Phase 8)

- Matmul correctness gates: PASS
- BLAS hybrid path: PASS
- Epilogue fusion path: PASS
- Pack/cache steady-state counters: PASS
- Minimal auto-tuner + tuned config cache: PASS
- Benchmark artifact production (JSON/CSV): PASS

## Deferred

- MatrixView gerçek stride-specialized kernel path (şu an contiguous materialized path ağırlıklı)
- Multi-thread scaling (Phase 9)
- Matmul autotune search-space genişletme + cost-model (Phase 8.5/9)

## Post-Report Optimization Addendum (2026-02-17)

- Own GEMM path now includes row-tile parallel execution (guarded by work-volume threshold).
- New controls:
  - `SPARK_MATMUL_OWN_THREADS`
  - `SPARK_MATMUL_OWN_PAR_MIN_VOLUME`

Latest benchmark snapshot after this change:
- `matmul_core_f64_256`: `c/own ~= 0.309` (previous typical band ~`0.247`)
- `matmul_core_f64_512`: `c/own ~= 0.404` (previous typical band ~`0.300`)
- Auto backend selection quality remained stable (`auto/selected` near `1.0` on 256/512).

Additional host tuning update:
- `arm_neon` dispatch profile now uses `j8=max`, `j4=128`, `thread_cap=12`, `min_volume=3*1024*1024`.
- Latest snapshot (same harness):
  - `matmul_core_f64_256`: `c/own ~= 0.302`
  - `matmul_core_f64_512`: `c/own ~= 0.407`
