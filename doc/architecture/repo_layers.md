# Repository Layer Map

Repository artık iki eksende tariflenir:

1. Fiziksel kod yerleşimi (`compiler/src/phase*`, `tests/phase*`, `scripts/core/pipeline/phase10`)
2. Mantıksal modül mimarisi (Core / Port / Application)

## Mantıksal Modül Mimarisi (Core / Port / Application)

- `core/`
  - `dto/`, `behavior/`, `mapper/`, `driver/`, `manager/`, `servis/`, `logic/`, `mode/`, `ui/`, `common/`
  - DTO alt sınıflama: `atom`, `molecule`, `compound`, `tissue`, `organ`, `system`, `organism`
- `port/`
  - `input/`, `output/`
- `application/`
  - `config/`, `wiring/`

Detay ve kurallar: `docs/architecture/core_port_application_model.md`

## Fiziksel Kod Yerleşimi

- `compiler/`
  - `include/spark/`: stable public interfaces.
  - `src/common/`: shared utilities.
  - `src/application/main.cpp`: Application module entrypoint.
  - `src/core/*`: Core module compatibility transition layer.
  - `src/phase1..phase9/`: mevcut derleyici/runtime katman uygulamaları.
    - `phase3`: semantic + evaluator
    - `phase4`: codegen
    - `phase5`: runtime primitive/core ops
    - `phase6`: hetero runtime
    - `phase7`: pipeline/fusion runtime
    - `phase8`: matmul runtime
    - `phase9`: concurrency runtime
- `tests/phase1..phase10/`: phase-scoped test suites.
- `bench/programs/phase*/`, `bench/scripts/`, `bench/results/`: benchmark hatları.
- `scripts/core/pipeline/phase10/`: platform/cpu/gpu matrix automation.

## Sert Mimari Kurallar

1. Bağımlılık tek yönlüdür: üst katman alt katmana iner, alt katman yukarı çıkmaz.
2. `port/input` ve `port/output` ile temas eden tek core katmanı `driver`dır.
3. DTO construct/new yetkisi `driver` katmanındadır.
4. Shared yardımcılar `common` içinde tutulur; aynı helper farklı katmanlarda kopyalanmaz.
5. Performans iddiası içeren değişikliklerde runtime-only ölçüm kanıtı zorunludur.

## CI Mimari Güvencesi

`.github/scripts/architecture_guard.py` aşağıdakileri doğrular:

1. Faz dizinleri + test dizinleri varlığı.
2. Core/Port/Application modül iskeleti varlığı.
3. Primitive kapsam marker’ları (`i8..i512`, `f8..f512`, `+,-,*,/,%,^`).
4. CI workflow wiring marker’ları (ctest + cross-language + perf + platform readiness).
5. Dosya satır bütçesi (`--max-lines`).

## Runtime Execution Modes

- `interpret`: correctness/debug odaklı yorumlayıcı akışı.
- `native`: derlenmiş runtime akışı.
- `builtin`: mikro-kernel stress yardımcı modu (tam dil performans iddiası için tek başına yeterli değil).
