# Phase 3 Type + Container Rules (Current Phase Baseline)

## 1) Tip sistemi (analiz görünümü)

`TypeChecker` mevcutta aşağıdaki temel tipleri üretir:

- `Int`, `Float(<precision>)` (`float` ailesi için `f64`, genişletme kuralları mevcut)
- `Bool`, `Any`
- `List[T]`, `List[Any]` (`[]` başlangıçta `List[Unknown]` ile takip edilir)
- `Matrix[T]`, `Matrix[Any]`
- `Class(name, open/slots)`
- `Function` ve `Builtin`

## 2) Kural: list/matrix element birleştirme (`normalize`)

Mevcut normalize mantığı:

- `List[int, int, ...]` => `List[Int]`
- `List[int, float]` => `List[Float]` (float açılımı)
- `List[int, bool]` veya `Any` geçen bir öğe => `List[Any]`
- `Matrix` satırları `ListExpr` olduğunda `Matrix[T]` olarak ele alınır.

`append` kuralları (phase3):

- `append` hedefin iç tipi `Unknown` ise eklenen eleman tipine göre daraltılır.
- `int` sonrasında `float` gelirse `List[Float]` oluşturmak, normalize edilebilir (`T5`) kabul edilir.
- `int` sonrasında uyumsuz bir tip gelirse (`bool` gibi) hard-yol (`T8`) sebebi üretir.

Kuralın etkisi: tip analizi sonrası `Any`/heterojen yapı `T5` adayını doğurur (normalize ile iyileştirme mümkünse).

## 3) Class/shape kuralları

- `class X(open): ...` -> açık şekil (`open=true`)
- `class X:` / `class X(slots): ...` -> kapalı şekil (`open=false`)

`shape` raporu:

- alan listesi (`fields`)
- açık/kapalı flag’i
- ek nedenler (`reasons`)  

Ana amaç: açık şekilli nesne erişimini T4’ten hariç tutup daha güvenli optimize yolunu belirtmek.

## 4) Tier karar kuralları (özet)

Her fonksiyon ve döngü için bir neden listesi tutulur:

- Hard neden (`normalizable = false`) -> `T8`
- Sadece normalize-yol sebebi (`normalizable = true`) -> `T5`
- Neden yok -> `T4`

Örnek hard nedenler:

- Bilinmeyen/uygunsuz çağrı (non-callable, arity uyuşmazlığı)
- Açık (`open`) sınıf alanı kullanımı veya dinamik davranış
- `Any` ile karışık tipte veya yazımsız liste elemanı

Örnek normalizable nedenler:

- `List[Any]` üzerinde sıcak kod bölgeleri
- `append` ile mutasyon yapan ama tek başına normalize edilebilecek senaryolar

## 5) CLI raporlama

- `sparkc analyze file.k` -> tier raporu
- `sparkc analyze file.k --dump-types` -> tip tablosu
- `sparkc analyze file.k --dump-shapes` -> shape tablosu
- `sparkc analyze file.k --dump-tiers` -> tier raporu (varsayılan)
