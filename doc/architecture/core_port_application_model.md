# Core / Port / Application Modeli

Bu belge, KozmikaDil mimarisini Core/Port/Application modeline taşıyan resmi referanstır.

## 1) Üst Modüller

1. `src/core/`
2. `src/port/`
3. `src/application/`

### Sorumluluk

1. `core`: iş kuralları, domain veri katmanları ve uygulama davranışları.
2. `port`: dış dünya giriş/çıkış kapıları (`input`/`output`).
3. `application`: entrypoint, config ve wiring.

## 2) Core Katmanları

Sıra ve kapsam:

1. `dto`
2. `behavior`
3. `mapper`
4. `driver`
5. `manager`
6. `servis`
7. `logic`
8. `mode`
9. `ui`

DTO biyolojik hiyerarşisi:

1. `atom`
2. `molecule`
3. `compound`
4. `tissue`
5. `organ`
6. `system`
7. `organism`

## 3) Hard Kurallar

1. Bağımlılık tek yönlüdür: üst katman altı çağırır, alt katman üstü çağırmaz.
2. Port ile konuşan tek core katmanı driver.
3. DTO construct/new sadece driver.
4. Her DTO en az `dto + mapper + driver` katmanına sahip olmalıdır.
5. Driver, DTO koleksiyonlarını çoğul ve ID-map odaklı tutar.

## 4) Port Sınırı

1. `port/input`: dış dünyadan veri/komut alma.
2. `port/output`: dış dünyaya yazma/yayınlama.
3. Core içinde `port` erişimi sadece `core/driver` içinde yapılır.

## 5) Application Sınırı

1. Main/entrypoint burada olur.
2. Config ve dependency wiring burada olur.
3. İş kuralı burada yazılmaz; sadece core akışları çalıştırılır.

## 6) Servis ve Logic Ayrımı

1. `servis`: tek use-case, transaction/idempotency/hata sınırı.
2. `logic`: birden fazla servisi süreç/workflow olarak orkestre eder.

## 7) Driver Standardı

Her driver şu yapıyı korur:

1. Dış kaynak girişleri (`fromPortA`, `fromPortB`, vb.)
2. Tek bir internal construct/builder
3. Mapper ile canonical ID-map state üretimi

## 8) Depo Uygulaması

Bu modelin fiziksel iskeleti aşağıdaki dizinlerde açılmıştır:

1. `src/core/`
2. `src/port/`
3. `src/application/`

Derleme seviyesi uyumluluk geçişi:

1. `compiler/src/application/main.cpp` giriş noktası
2. `compiler/src/core/driver/*` parser/lexer uyumluluk
3. `compiler/src/core/manager/*` semantic uyumluluk
4. `compiler/src/core/logic/*` evaluator uyumluluk
5. `compiler/src/core/servis/*` codegen uyumluluk

CI guard (`.github/scripts/architecture_guard.py`) bu iskeletin ve ana kuralların varlığını doğrular.

## 9) Geçiş Köprüsü (Phase Bridge) Kuralı

Phase implementasyonlarına erişim sadece izinli core köprü dosyaları üzerinden yapılır:

1. `compiler/src/core/driver/parser.cpp` -> `phase2/parser.cpp`
2. `compiler/src/core/driver/lexer.cpp` -> `phase2/lexer.cpp`
3. `compiler/src/core/manager/semantic.cpp` -> `phase3/semantic.cpp`
4. `compiler/src/core/logic/evaluator.cpp` -> `phase3/evaluator.cpp`
5. `compiler/src/core/servis/codegen.cpp` -> `phase4/codegen.cpp`

Hard boundary:

1. `compiler/src/application/*` içinde `phase*` include yasaktır.
2. `compiler/src/port/*` içinde `phase*` include yasaktır.


## Geçiş Uyumluluk Notu
- compiler/ dizini sadece geçiş/uyumluluk (compatibility) katmanıdır; gerçek kaynak kökleri src/ ve include/ dizinleridir.
