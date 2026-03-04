# kozmikaDil Proje Dosyası

## Proje İsmi
- kozmikaDil

## Amaç
- KodX (compiler/runtime) tabanlı bir dil ve performans odaklı çalışma hattını tek çatı altında toplamak.

## Mevcut Klasör Yapısı
- `compiler/` : Derleyici çekirdeği ve aşamalar
- `runtime/` : Çalışma zamanları ve destek kütüphaneleri
- `stdlib/` : Standart kütüphane
- `tests/` : Birim ve entegrasyon testleri
- `bench/` : Performans ölçümleri
- `docs/` : Dokümantasyon
- `scripts/` : Otomasyon ve yardımcı betikler

## Başlıca Komutlar
- Derleme: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` ardından `cmake --build build -j4`
- Çalıştırma: `./k run examples/main.k` (varsa örnek dosyasıyla)
- Test: `cmake -S . -B build -DSPARK_BUILD_TESTS=ON && cmake --build build -j4`

## Notlar
- Repo şu an çoklu benchmark ve sonuç dosyasıyla büyük olduğundan `build/` ve `bench/results/` klasörleri dikkatle yönetilmeli.
- Yeni bir proje iş akışı eklenirse önce `README.md` güncellenecek.
