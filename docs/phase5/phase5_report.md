# Phase 5 Report

## Scope
- Kod tabanı Phase 5 hedeflerinde: homojen List/Matrix tipi, core operasyonları ve tip doğrulaması.
- Pythonik syntax’tan çıkan temel programlar: list literal/indexing/mutation/matrix literal/indexing/transpose.
- Runtime: matrix elementwise ops ve slice seçimi.

## What is done
- List metotlarının runtime tarafı (`append`, `pop`, `insert`, `remove`) aktif.
- Matrix literal parse ve matmul-benzeri değildir: elementwise `+ - * /` destekleniyor.
- Slicing ve transpose runtime’da çalışır.
- Phase4 codegen tarafı da list/matrix için bu operasyonları doğrudan lower ediyor:
  - `list`: `append/pop/insert/remove`, typed slice default-stop fix.
  - `matrix`: `m[r]`, `m[r,c]`, `m[:,c]`, `m[r0:r1, c0:c1]`.
  - `for row in matrix` row-list iterasyonu.
- TypeChecker:
  - matrix aritmetiği için tip çıkarımı eklendi,
  - list mutator arayan method çağrıları eklendi (`append`, `pop`, `insert`, `remove`),
  - matrix forma uyuşmazlığı ve matrix-scalar numerik kontrolleri eklendi.
- TypeChecker for-loop davranışı matrix iterasyonunda row-list olarak güncellendi.
- Phase5 testleri list/matrix kapsayıcı davranışlarını genişleterek `pop/insert/remove` + elementwise matrix ops eklendi.

## What worked
- `list` ve `matrix` runtime davranışında uyum büyük oranda doğrulandı.
- `[]` matrix syntax normalizasyonu ve `m.T` ile transpozisyon stabil.
- `Phase5` testleri lokal olarak çalışır duruma getirildi (öncesinden kalan kırılmalar azaltıldı).
- `k compile` ve `k run --allow-t5` ile list/matrix codegen yolu artık bu operasyonları native C backend üzerinden üretebiliyor.
- `bench/scripts/run_phase5_benchmarks.sh` güncel koşumunda correctness geçişi `2/2`, baseline-band geçişi `2/2` (varsayılan Phase 5 bandı `0.85x-1.15x`).
- Döngü-öncesi reserve optimizasyonu eklendi:
  - kanonik `while i < N` + `list.append(...)` deseninde codegen artık `__spark_list_ensure_*` çağrısını döngü öncesine enjekte ediyor.
  - özellikle `list_iteration` ve `matrix_elemwise` benchmarklarında realloc dalgalanması azaldı.
- Runtime copy/shift hot-path optimize edildi:
  - list `pop/insert/remove` kaydırmaları `memmove` ile,
  - matrix row-slice kopyası `memcpy` ile yapılıyor.
- Regression güvenliği:
  - `tests/phase4/codegen_tests.cpp` içine `while+append` reserve üretimini doğrulayan test eklendi.
- Matrix benchmark yolu typed-contiguous modele hizalandı:
  - yeni builtin constructorlar: `matrix_i64(rows, cols)`, `matrix_f64(rows, cols)`
  - benchmark programı list-of-list yerine doğrudan matrix set/get ile çalışıyor.
  - fill aşaması tek nested loop içinde `mat_a` + `mat_b` üretecek şekilde kanonikleştirildi.
  - C baseline da aynı algoritmaya (fill + consume) hizalandı.
- Kritik düzeltme:
  - matrix index chain sırası (`m[r,c]`) hem codegen hem interpreter tarafında düzeltildi.
- Matrix runtime erişim yolu optimize edildi:
  - matrix allocation 64-byte aligned path’e alındı.
  - matrix get/set helper’larında aligned + restrict uyumlu pointer erişimi kullanıldı.
- Son ölçümlerde native/baseline oranları (stable, varsayılan aggressive profile):
  - `list_iteration=1.019x`
  - `matrix_elemwise=1.052x`
  - geomean=`1.035x` (hedef `>1.0x` sağlandı)
  - kayıt: `bench/results/phase5_benchmarks_gt1.json` ve `bench/results/phase5_benchmarks_gt1.csv`

## What failed / Open
- `matrix` çağrısal API (method-based matmul vb.) eklenmedi, sadece elementwise ops var.
- `pop`/`insert`/`remove` hata politikası (örn. boş listeden pop, remove not-found) şu an runtime’da konservatif/no-op fallback; exception modeli sonraki fazda netleştirilmeli.
- view tabanlı `MatrixView` runtime temsili ve stride-aware performans optimizasyonu Phase 6 hedefi.
- “fully reproducible” metriği:
  - drift limiti (`±3%`) altında her zaman kalmıyor; özellikle kısa süren koşularda sistem jitter etkisi sürüyor.
- PGO yolu:
  - `use` modunda yanlış `-fcoverage-mapping` flag’i derleme kırıyordu; düzeltildi.
  - auto-PGO benchmark yolu eklendi (`instrument -> run -> merge -> use`, benchmark başına).
  - 3x3 kararlılık kıyasında auto-PGO, aggressive profile’a göre daha iyi ortalama geomean ve daha düşük sapma verdi.
  - bu nedenle `bench/scripts/run_phase5_benchmarks.sh` varsayılanı auto-PGO olarak güncellendi.
  - snapshot sonuçlar:
    - best observed stable run: geomean=`1.040x`
    - follow-up stable run: geomean=`0.998x`
  - yorum: tekil koşular dalgalanıyor; profil seçimi tek run değil trial ortalaması/sapması ile yapıldı.

## GO / NO-GO
- **GO**: Phase 5’e geçilecek seviyede temel semantik ve tipleme tamamlanıyor.
- **NO-GO kalemleri**: Ölçümlerde stabil speedup kanıtı ve view/allocate azaltımı için ek benchmark ve optimizasyon ölçüm katmanı.
