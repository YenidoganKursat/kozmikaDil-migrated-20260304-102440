# Core Module

Core, iş kuralı ve domain katmanlarını barındırır.

## Katmanlar

1. `dto`
2. `behavior`
3. `mapper`
4. `driver`
5. `manager`
6. `servis`
7. `logic`
8. `mode`
9. `ui`
10. `common`

## Sert Kurallar

1. Port ile konuşan tek core katmanı `driver`dır.
2. DTO construct/new sadece `driver` içinde yapılır.
3. Alt katman üst katmanı çağırmaz.
4. DTO hiyerarşisi `atom -> molecule -> compound -> tissue -> organ -> system -> organism` şeklindedir.

## Uygulama Notu

1. Çalışan derleyici geçiş DTO/behavior sözleşmeleri:
`/Users/kursatyenidogan/Documents/kozmikaDil/include/spark/core/dto/compiler_pipeline_dto.h`
2. Uyumluluk kaynakları yeni katmanlardan derlenir:
`/Users/kursatyenidogan/Documents/kozmikaDil/src/core/*`
