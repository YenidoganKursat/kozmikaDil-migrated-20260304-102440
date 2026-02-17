# Phase 7 Report

## Scope

Phase 7 hedefi: method-chain pipeline’larını (`map/filter/zip/reduce/scan`) tek execution plan altında fuse edip intermediate allocation’ları azaltmak.

## Implemented Deliverables

### 1) Pipeline execution runtime (Phase 7)

Yeni modüller:

- `compiler/src/phase7/runtime/01_pipeline_types.cpp`
- `compiler/src/phase7/runtime/02_pipeline_parse.cpp`
- `compiler/src/phase7/runtime/03_pipeline_exec_list.cpp`
- `compiler/src/phase7/runtime/04_pipeline_exec_matrix.cpp`
- `compiler/src/phase7/runtime/05_pipeline_entry.cpp`

Aktif davranış:

- AST method-chain parse -> pipeline chain.
- Fused path + non-fused fallback.
- Heterojen list path için plan-aware fusion (`reduce_sum` terminalinde).
- Cache/stats sayaçları (`pipeline_stats`, `pipeline_plan_id`).

### 2) Semantic diagnostics and dumps

TypeChecker’a pipeline kayıtları eklendi:

- `dump_pipeline_ir()`
- `dump_fusion_plan()`
- `dump_why_not_fused()`

CLI:

- `k analyze --dump-pipeline-ir`
- `k analyze --dump-fusion-plan`
- `k analyze --why-not-fused`

### 3) Test coverage

Yeni phase7 test target:

- `sparkc_phase7_tests`

Dosyalar:

- `tests/phase7/list/pipeline_list_tests.cpp`
- `tests/phase7/matrix/pipeline_matrix_tests.cpp`
- `tests/phase7/analyze/pipeline_analyze_tests.cpp`

Doğrulanan başlıklar:

- fused list pipeline correctness
- `SPARK_PIPELINE_FUSION=0` fallback correctness
- hetero steady-cache behavior
- matrix pipeline correctness
- pipeline dump içerikleri ve why-not-fused diagnostikleri

### 4) Benchmark pipeline (Phase 7)

Yeni modüler benchmark hattı:

- `bench/scripts/phase7/definitions.py`
- `bench/scripts/phase7/utils.py`
- `bench/scripts/run_phase7_benchmarks.py`
- `bench/scripts/run_phase7_benchmarks.sh`
- `bench/programs/phase7/*.k`

Çıktılar:

- `bench/results/phase7_benchmarks.json`
- `bench/results/phase7_benchmarks.csv`

## Benchmark Snapshot (runs=7, warmup=1, sample_repeat=3)

- `list_pipeline_reduce`: speedup `3.874x` (fused vs non-fused)
- `hetero_pipeline_reduce`: speedup `2.284x`
- `matrix_pipeline_reduce`: speedup `3.604x`

Phase 7 late-pass optimizations:

- pipeline receiver value kopyası kaldırıldı (list/matrix payload copy overhead düşürüldü)
- `PackedInt` reduce için doğrudan integer fast path eklendi
- küçük transform zincirleri için inner-loop dispatch sadeleştirildi
- `map_mul -> reduce_sum` için list/matrix fused fast-path eklendi
- `map_add/map_mul` affine zincirleri için reduce formülü (tek-pass toplam + sabit düzeltme) eklendi

Allocation side:

- fused modda `last_allocations=0`
- non-fused modda allocation gözleniyor (`1-3` aralığı, programa göre)

## Exit Criteria Check (Phase 7)

- Pipeline IR + fusion plan dump: PASS
- Why-not-fused diagnostics: PASS
- Fused execution path (list + matrix subset): PASS
- Differential behavior (`fusion on/off` same checksum): PASS
- Fused vs non-fused measurable speedup: PASS (`1.8x-3.6x` bandı)
- Allocation reduction: PASS (fused path zero-last-allocation)

## Deferred / Next

- Full native codegen’de generic lambda/comprehension fusion (Phase 8+ genişletme).
- Random-access ağır pipeline’larda gelişmiş materialization policy.
- `perf stat` Linux counter entegrasyonunun phase7 scriptine opsiyonel eklenmesi.
