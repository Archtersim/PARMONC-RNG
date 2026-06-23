# Пакет для воспроизведения результатов PARMONC RNG и SEIRD

Этот пакет, подготовленный для публикации в репозитории, собирает исходные файлы, сценарии бенчмарков, обёртки для тестов качества и ключевые таблицы результатов, использованные в исследовании PARMONC v1-v5 и экспериментах по интеграции в SEIRD-модель.

## Структура

- `src/` - автономные реализации `v1-v5`
- `seird/` - код SEIRD-модели
- `bench/` - исходники автономных бенчмарков и тестов пропускной способности памяти
- `tests/` - обёртки для TestU01 / PractRand
- `scripts/` - PowerShell-сценарии запуска
- `results/` - выбранные таблицы результатов и сводки
- `results/env/` - сведения о машине и инструментальной цепочке, использованных при бенчмарках
- `docs/` - краткие поясняющие заметки

## Соответствие версий

- `v1` - эталонная реализация PARMONC
- `v2` - упакованное 128-битное состояние с ручной реализацией умножения 64x64 -> 128
- `v3` - нативная 128-битная арифметика / intrinsics
- `v4` - CPU-версия с 4-way ILP, где AVX2 используется для упаковки и записи
- `v5` - CUDA-реализация

## Рекомендуемый путь воспроизведения

### 1. Сборка автономных CPU-версий

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_standalone_v1_v4.ps1
```

### 2. Сборка SEIRD-версий

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_seird_strict_v1.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\build_seird_strict_v34.ps1
```

### 3. Запуск основного SEIRD-бенчмарка для новой ветки

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\bench_openmp.ps1 `
  -Exe .\seird\seird_v4_STRICT.exe `
  -Nr 100 `
  -Tmod 90 `
  -Block 1000000 `
  -ThreadsList "1,16"
```

### 4. Сравнение старой и новой ветвей при 16 потоках

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\bench_unified_seird_old_new_t16.ps1
```

### 5. Запуск специального режима `model1_rng16` ускорение ГПСЧ в 1 потоке

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_model1_rng16_v1_v4.ps1
```

### 6. Запуск тестов качества

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\run_testu01_matrix.ps1
```

Для PractRand используйте `tests/rng_streamer.cpp` / `bench/rng_streamer.cpp` и примеры итоговых сводок из `results/practrand/`.

## Примечания

- Для `v5` требуются CUDA Toolkit и рабочий host-компилятор MSVC для `nvcc`.
- В `results/` собраны выбранные сводки, а не все сырые прогоны, полученные в ходе работы.
- В `scripts/extra/` лежат несколько более поздних вспомогательных сценариев, пригодившихся при сравнительных прогонах.
