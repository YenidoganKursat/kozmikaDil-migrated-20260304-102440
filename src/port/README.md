# Port Module

Port, Core'un dış dünya ile entegrasyon kapısıdır.

## Katmanlar

1. `input`: dış sistemden okuma/alma adaptörleri.
2. `output`: dış sisteme yazma/yayınlama adaptörleri.

## Kural

Core içinde `port` kullanım yetkisi yalnızca `core/driver` katmanındadır.
