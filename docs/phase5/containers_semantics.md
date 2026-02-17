# Phase 5 Container Semantics

Bu dokümanda Phase 5 için List/Matrix davranışı, normalize kuralları ve runtime semantiği sabitlenir.

## List Semantiği

- `x = []`
  - Tip çözümleme: `List[Unknown]`.
- `append`:
  - `x.append(v)` yalnızca `List` üzerinde geçerli.
  - `v` ilk eleman tipini belirler; sonraki eklemelerle `normalize` ile genişler.
- `pop`:
  - `x.pop()` son elemanı çıkarır.
  - `x.pop(i)` pozisyonlu hali (negatif indeks desteklenir).
- `insert`:
  - `x.insert(i, v)` ile `i` indeksine değer ekler.
- `remove`:
  - `x.remove(v)` ilk eşleşen `v` değerini kaldırır, bulunamazsa hata verir.
- `slice`:
  - `x[a:b]` liste dilimi (şu andaki runtime’da yeni liste üretir).
- `indexing`:
  - `x[i]` hem okunur hem yazılabilir.
  - `x[a:]` stop değeri varsayılan olarak `len(x)` kabul edilir.
- list + list:
  - `a + b` concat (tip: `List[T]`).

## Matrix Semantiği

- Matrix constructor:
  - `matrix_i64(rows, cols)` -> `Matrix[Int]` ve başlangıç değeri `0`.
  - `matrix_f64(rows, cols)` -> `Matrix[Float(f64)]` ve başlangıç değeri `0.0`.
- Boyutlar:
  - constructor argümanları negatif olamaz.
- Matrix literal:
  - `[[...], [...]]` ve `[[...]; [...]]` parser ile tek canonical AST’e normalize edilir.
  - Tüm satır sayısı ve sütun sayısı eşit değilse hata.
- Tür:
  - Satırlar homojense ve en az bir eleman float ise `Matrix[Float(f64)]`; aksi takdirde ortak tip.
- indexleme:
  - `m[r][c]`, `m[r, c]`, `m[r,:]`, `m[:, c]`, `m[r1:r2, c1:c2]`.
  - `m[r]` bir satır listesi döndürür.
- transpose:
  - `m.T` ile `Matrix[cols, rows]` dönüşümü.
- elementwise:
  - `+ - * /` desteklenir.
  - `mod` desteklenmez.
- view:
  - `m.T`, satır/sütun ve blok seçimi yeni matris döndürür (view optimizasyonu Phase 5 planında tasarım olarak notlandı, runtime’da fiziksel kopya ile)
  (plan hedefi: ileride view metadatasını koruyup cost model’e bırakmak).
- `for row in m`:
  - iterasyon bir satır listesi (`List[T]`) üzerinde yapılır, scalar eleman üzerinde değil.

## TypeChecker Davranışı (Phase 5)

- list metotları (`append`, `pop`, `insert`, `remove`) arayan çağrılar tiplenir.
- `pop` dönüşü list element tipidir.
- matrix aritmetiğinde elemanlar numeric olmalı; scalar-matrix veya matrix-matrix ikilisi desteklenir.
- şekil uyuşmazlığı (bilinen satır/sütunlarda) Type error üretir.
