# Phase 7 Pipeline Semantics

## Scope

Phase 7 hedefi: list/matrix method-chain pipeline’larını mümkün olduğunda fuse ederek ara container (intermediate) üretimini kaldırmak.

Bu sürümde aktif pipeline stage seti:

- `map_add(scalar)`
- `map_mul(scalar)`
- `filter_gt(scalar)`
- `filter_lt(scalar)`
- `filter_nonzero()`
- `zip_add(list)`
- `reduce_sum()` / `sum()`
- `scan_sum()`
- `to_list()` / `collect()`

Matrix tarafında Phase 7 minimum kapsam:

- `map_add(scalar)`
- `map_mul(scalar)`
- `reduce_sum()`
- `to_list()`

## PIR ve Fusion Görünürlüğü

Analyze çıktıları:

- `--dump-pipeline-ir`
- `--dump-fusion-plan`
- `--why-not-fused`

Bu raporlar pipeline node zincirini, fuse kararı (`fused=yes/no`) ve gerekçeleri verir.

## Fusion Kuralları

### Fusable fast path

- Homojen `Packed[Int|Double]` list/matrix.
- Heterojen numeric list için `GatherScatter`/`ChunkedUnion` planı ile `reduce_sum()` terminali.
- `SPARK_PIPELINE_FUSION=1` (varsayılan).

### Fusion barrier (bilerek)

- `BoxedAny` layout.
- Heterojen plan + order-sensitive terminal (`to_list`, `scan_sum`) kombinasyonu.
- `zip_add` ile heterojen cached path kombinasyonu.
- Mutation içeren chain (`append/pop/insert/remove` geçişleri) analyze katmanında non-fused raporlanır.

Karar: **perf-tier (T4) için random-access ara adımlar fusion barrier** kabul edilir.
Bu, planı daha öngörülebilir ve stabil tutar.

## Materialization Policy

- `Packed*` path: materialize yok.
- `PromotedPackedDouble`: numeric buffer tek sefer materialize.
- `GatherScatter` / `ChunkedUnion`: numeric projection buffer tek sefer materialize.
- Cache anahtarı: receiver + version + pipeline signature.
- Mutasyon sonrası (`append/insert/remove/index write`) cache version invalid olur.

## Correctness Rules

- Fast-math yok, strict arithmetic.
- Pipeline optimization yalnızca semantik korunuyorsa aktif.
- Fused çalışamazsa otomatik non-fused path ile aynı sonucu üretir.
- Differential yaklaşım: aynı program için fused (`SPARK_PIPELINE_FUSION=1`) ve non-fused (`=0`) checksum karşılaştırılır.

## Runtime Introspection

`pipeline_stats()` dönen sayaç sırası:

1. `analyze_count`
2. `materialize_count`
3. `cache_hit_count`
4. `fused_count`
5. `fallback_count`
6. `last_allocations`
7. `total_allocations`
8. `plan_id`

`pipeline_plan_id()`:

- Son görülen pipeline plan kimliği (`LayoutTag`).
