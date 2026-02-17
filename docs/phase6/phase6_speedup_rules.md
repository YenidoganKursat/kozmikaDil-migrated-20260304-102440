# Phase 6 Speedup Rules (Research + Applied Plan)

## Amaç

Phase 6 hedefi, heterojen container semantiğini korurken hot loop tarafında per-element tag branch maliyetini steady-state'te minimize etmektir.

## Bu turda uygulananlar

- `live_plan` tabanlı invalidation:
  - Plan/cache aktif değilken (`append` gibi) invalidation kısa devre edilir.
  - Amaç: inşa aşamasındaki gereksiz invalidation işini kesmek.
- `ChunkedUnion` için numeric yan-cache:
  - Chunk planında numeric index/değerler materialize edilir.
  - `reduce_sum/map_add` tekrarlarında tekrar tag dönüşümü azaltılır.

## Bu turda denenip reddedilen

- `PackedInt/PackedDouble` için ek cache buffer'larını doğrudan `Value::ListCache` içinde tutma denemesi:
  - Sonuç: `Value` boyutu şiştiği için list elemanı taşıma/okuma maliyeti arttı.
  - Karar: bu yaklaşım geri alındı; out-of-line metadata olmadan tekrar denenmeyecek.

## Araştırmadan çıkan net kurallar

Olumlu (yap):

- Hot path'te tek-dispatch: `analyze -> plan` kararını loop dışına taşı.
- Numeric hetero için bir kez promote et, steady-state'te unboxed buffer kullan.
- Chunk/gather seçiminde erişim desenine göre plan seç:
  - iterasyon ağırlıklı: `ChunkedUnion`
  - order-kritik ve numeric yoğun: `GatherScatter`
- Cache invalidation'ı epoch/version ile yönet; aynı epoch'ta tekrar materialize etme.
- Ölçümleri first-run ve steady-run ayrı tut; kararları sadece steady-state ortalamasına göre kilitle.

Negatif (yapma):

- `Value` gibi her elemanda taşınan yapıları büyük metadata ile şişirme.
- Tek run'a bakıp profil/plan lock etme.
- Hetero path için hot loop içinde per-element type switch bırakma.
- Semantik değiştiren agresif numerik bayrakları (fast-math vb.) varsayılan açma.

## Öncelikli backlog (uygulanacak)

1. `Value` footprint küçültme (P0):
   - List/matrix cache yapısını out-of-line (pointer-handle) taşı.
   - Beklenen etki: container iteration ve append taşıma maliyetinde kalıcı düşüş.
2. PIC (polymorphic inline cache) benzeri call-site cache (P0):
   - `operation + layout + mutation_epoch` anahtarına göre doğrudan kernel çağrısı.
   - Beklenen etki: `ensure_*_plan` branch/karar maliyeti düşer.
3. Incremental cache update (P1):
   - `append/pop/set` için tam rematerialize yerine kısmi güncelleme.
   - Beklenen etki: mutation sonrası ilk-run maliyetinde düşüş.
4. Chunk metadata sıkıştırma (P1):
   - Uzun run'lar için run-end/dictionary benzeri temsil.
   - Beklenen etki: cache_bytes ve cache-miss düşüşü.
5. Codegen tarafında attribute taşıma (P1):
   - `nonnull`, `noundef`, `dereferenceable`, `noalias` uygun yerlerde üret.
   - Beklenen etki: LLVM'nin LICM/GVN/vectorizer başarımı artar.

## Ölçüm protokolü (kilit)

- En az 7 örnek, trim-mean/median ile rapor.
- First-run vs steady-run zorunlu ayrı rapor.
- Karar metriği:
  - `steady_vs_packed`
  - `cycles/elem`
  - `branch-misses/elem`
  - `cache-misses/elem`
  - `allocs/op`

## Kaynaklar

- [LLVM LangRef](https://llvm.org/docs/LangRef.html)
- [LLVM Vectorizers](https://llvm.org/docs/Vectorizers.html)
- [V8 Hidden Classes](https://v8.dev/docs/hidden-classes)
- [PyPy Interpreter Optimizations](https://doc.pypy.org/en/latest/interpreter-optimizations.html)
- [Julia isbits Union Optimizations](https://docs.julialang.org/en/v1/devdocs/isbitsunionarrays/)
- [Apache Arrow Columnar Format](https://arrow.apache.org/docs/format/Columnar.html)
- [DuckDB Vector Format](https://duckdb.org/docs/stable/internals/vector)
- [CPython listobject.c](https://raw.githubusercontent.com/python/cpython/main/Objects/listobject.c)
- [Weld Paper (CIDR)](https://cs.stanford.edu/~matei/papers/2017/cidr_weld.pdf)
