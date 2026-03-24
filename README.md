# parallel-containers-bench

Научно-ориентированный бенчмарк-фреймворк для сравнения параллельных контейнеров (lock-free и fine-grained locking хеш-таблицы и map'ы).

## Ключевые принципы

- **Нулевой overhead в горячем цикле**: все аллокации, генерация ключей, подготовка операций — строго до старта замера. Во время основного цикла нет вызовов Lua, `rand()`, `new/delete`, `std::cout`.
- **Воспроизводимость**: детерминированные seed'ы для ГПСЧ, thread pinning (CPU affinity), rdtsc для latency.
- **Прозрачность**: полные метрики в CSV, доверительные интервалы по t-распределению Стьюдента.
- **Гибкость**: конфигурация через Lua, поддержка пользовательских контейнеров через CRTP.

## Быстрый старт

```bash
git clone <repo>
cd parallel-containers-bench
chmod +x setup.sh
./setup.sh
./build/bench config/default.lua
```

## Контейнеры

| Контейнер      | Тип                               | Библиотека             |
| -------------- | --------------------------------- | ---------------------- |
| SplitListMap   | Lock-free (split-ordered list)    | libcds                 |
| FeldmanHashMap | Lock-free (hash trie)             | libcds                 |
| CK_HT          | Lock-free reads / spinlock writes | Concurrency Kit        |
| ShardedHashMap | Fine-grained locking (256 шардов) | Собственная реализация |

## Конфигурация

Файл `config/default.lua` определяет:

- Список потоков (`threads_list`)
- Контейнеры с бюджетом времени (`target_time_minutes`)
- Сценарии нагрузки с фазами (пропорции insert/find/erase, распределение ключей)

## Метрики

- **Throughput**: ops/sec (суммарный по всем потокам)
- **Latency**: p50, p95, p99, p99.9 (rdtsc тики или наносекунды)
- **Системные**: CPU utilization, RSS, context switches, page faults, LLC cache misses
- **Статистика**: mean, stddev, min, max, 95% CI

## Вывод

- `results/raw_results.csv` — все прогоны
- `results/summary.csv` — агрегация по контейнерам
- `results/system_metrics.csv` — системные метрики с временными метками

## Структура проекта

```
parallel-containers-bench/
├── CMakeLists.txt
├── main.cpp
├── setup.sh
├── config/
│   └── default.lua
├── include/
│   ├── workload/
│   │   ├── WorkloadGenerator.hpp
│   │   ├── ZipfianGenerator.hpp
│   │   └── Phase.hpp
│   ├── containers/
│   │   ├── ContainerAdapter.hpp
│   │   ├── CDS_Adapters.hpp
│   │   ├── CK_HT_Adapter.hpp
│   │   └── CustomAdapter.hpp
│   ├── executor/
│   │   └── MultiThreadExecutor.hpp
│   ├── metrics/
│   │   ├── SystemSampler.hpp
│   │   └── Statistics.hpp
│   └── output/
│       └── CsvWriter.hpp
└── README.md
```

## Зависимости

- C++20 compiler (GCC ≥ 12, Clang ≥ 15)
- CMake ≥ 3.22
- [libcds](https://github.com/khizmax/libcds) — lock-free контейнеры
- [Concurrency Kit](https://github.com/concurrencykit/ck) — ck_ht
- [sol2](https://github.com/ThePhD/sol2) + Lua 5.3/5.4 — конфигурация
- [fmt](https://github.com/fmtlib/fmt) + [spdlog](https://github.com/gabime/spdlog) — логирование

## Ссылки

1. Cooper et al., "Benchmarking Cloud Serving Systems with YCSB", SoCC 2010
2. Shalev & Shavit, "Split-Ordered Lists: Lock-Free Extensible Hash Tables", JACM 2006
3. Feldman et al., "A Wait-Free Hash Map", JPDC 2013
4. Hörmann & Derflinger, "Rejection-inversion to generate variates from monotone discrete distributions", ACM TOMACS 1996
5. Intel SDM Vol. 3B, Ch. 17.17 — "Time-Stamp Counter"

## Лицензия

MIT

TODO: убрать target_time_minutes, target_total_time_minutes,
