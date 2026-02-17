# Phase 6 Container Semantics (v2)

Bu doküman Phase 6 için heterojen list/matrix davranışını, state-machine geçişlerini ve cache/invalidation kurallarını sabitler.

## 1) List v2: tek semantik tip, çoklu fiziksel temsil

`List` semantik olarak tek tiptir; runtime planlayıcı operasyona göre şu fiziksel yolu seçer:

- `PackedInt` (`plan=1`): homojen `int`, unboxed contiguous.
- `PackedDouble` (`plan=2`): homojen `double`, unboxed contiguous.
- `PromotedPackedDouble` (`plan=3`): `int+double` heterojen numeric tek seferde `f64` buffer’a normalize edilir.
- `ChunkedUnion` (`plan=4`): type-run segmentleri çıkarılır; run başına tek dispatch, run içinde saf loop.
- `GatherScatter` (`plan=5`): numeric indeksler tek seferde gather edilir; hot path numeric buffer üzerinden çalışır.
- `BoxedAny` (`plan=6`): tam genel fallback.

## 2) Analyze -> Plan -> Materialize -> Cache

`reduce_sum()` ve `map_add()` çağrılarında:

1. `analyze`: container layout okunur.
2. `plan`: operasyon için uygun fiziksel yol seçilir.
3. `materialize`: gerekiyorsa yardımcı buffer/chunk yapısı üretilir.
4. `cache`: aynı container + aynı operasyon için plan tekrar kullanılır.

`cache_stats()` sırası:
- `[analyze_count, materialize_count, cache_hit_count, invalidation_count, version, plan]`

Ek gözlemlenebilirlik:
- `cache_bytes()` -> materialize edilmiş yardımcı veri footprint’i (byte).

## 3) Heterojen operasyon semantiği

### `reduce_sum()`

- Packed yollar: standart sayısal toplam.
- `PromotedPackedDouble`: normalize edilmiş `f64` buffer üzerinden toplar.
- `GatherScatter`: gather edilen numeric altküme üzerinden toplar.
- `ChunkedUnion`: numeric run’ları toplar; non-numeric run’ları atlar.
- `BoxedAny`: numeric elemanları toplar; non-numeric elemanları atlar.

Not: Bu davranış Phase 6’da “heterojen container her durumda çalışsın” hedefi için seçildi.

### `map_add(delta)`

- Numeric hücrelere `+delta` uygulanır.
- Nonnumeric hücreler korunur (sıra bozulmaz).
- Çıktı listesi numeric hücrelerde `f64` üretir.
- `GatherScatter/ChunkedUnion` planlarında hot path per-element tag dalını azaltmak için cache’li metadata kullanılır.

## 4) Cache invalidation kuralları

Mutasyon sonrası cache invalid olur:

- list: `append`, `pop`, `insert`, `remove`, indexed `set` (`x[i]=...`)
- matrix: indexed `set` (`m[r,c]=...` ve türevleri)

Invalidation etkisi:

- `version` artar.
- `analyzed_version/materialized_version` resetlenir.
- materialize buffer’ları temizlenir (`cache_bytes()` sıfıra iner).
- Not: aktif plan/cache yoksa invalidation kısa devre edilir (inşa aşamasında gereksiz işten kaçınma).

## 5) Matrix v2 (Phase 6 minimum)

- Hetero numeric matrix literal (`int+double`) literal aşamasında `Matrix[f64]` olarak normalize edilir.
- Bu nedenle numeric-mixed literal çoğunlukla `PackedDouble` planına girer.
- Hetero object matrix `BoxedAny` (`plan=6`) üzerinde kalır.
- `reduce_sum()` numeric olmayan hücre görürse matrix tarafında hata üretir (correctness-first politika).

## 6) Bilerek ertelenenler

- Matrix için ChunkedUnion/GatherScatter fiziksel kernel varyantları.
- Hetero matrix için tam normalize-plan-cache operasyon seti (Phase 6.5+/7+).
- Pipeline fusion ve matmul schedule optimizasyonları (Phase 7/8).
