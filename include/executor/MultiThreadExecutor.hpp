#pragma once
// ============================================================================
// MultiThreadExecutor.hpp — оркестрация многопоточного выполнения бенчмарка.
//
// Архитектура:
//   1. Предвычисление операций (WorkloadGenerator) — ДО старта замера
//   2. Барьер (std::barrier) — все потоки стартуют одновременно
//   3. Горячий цикл: каждый поток итерирует по своему vector<Op>
//      - Нулевые аллокации, нулевые syscall'ы (кроме rdtsc)
//      - Latency: rdtsc до/после каждой операции → thread-local гистограмма
//   4. Барьер — все потоки завершаются
//   5. Сбор результатов: merge гистограмм, вычисление throughput
//
// Thread pinning:
//   sched_setaffinity() привязывает поток к конкретному ядру.
//   Это критически важно для воспроизводимости: без pinning'а OS scheduler
//   может мигрировать поток между ядрами → cache pollution → шум.
//
// rdtsc vs chrono:
//   rdtsc — ~25 циклов на вызов (constant_tsc на modern x86).
//   chrono::steady_clock — ~50-100 нс (vDSO, но всё равно дороже).
//   Для latency отдельных операций rdtsc предпочтительнее.
//
// Ссылки:
//   [7] Intel SDM Vol. 3B, Ch. 17.17 — "Time-Stamp Counter"
//   [8] Drepper, "What Every Programmer Should Know About Memory", 2007
// ============================================================================

#include "containers/ContainerAdapter.hpp"
#include "metrics/Statistics.hpp"
#include "metrics/SystemSampler.hpp"
#include "output/CsvWriter.hpp"
#include "workload/Phase.hpp"
#include "workload/WorkloadGenerator.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace bench {

// ---------------------------------------------------------------------------
// rdtsc intrinsic
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
inline uint64_t rdtsc() {
  uint32_t lo, hi;
  // RDTSCP сериализует: гарантирует, что все предыдущие инструкции завершены.
  asm volatile("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
  return (static_cast<uint64_t>(hi) << 32) | lo;
}
#else
// Fallback: chrono (менее точный, но портативный)
inline uint64_t rdtsc() {
  return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}
#endif

// ---------------------------------------------------------------------------
// Per-thread result
// ---------------------------------------------------------------------------

/// Результат работы одного потока за один прогон.
struct alignas(64) ThreadResult {
  uint64_t ops_completed = 0;
  std::vector<uint64_t> latencies; ///< в тиках rdtsc или нс
};

// ---------------------------------------------------------------------------
// Pin thread to core
// ---------------------------------------------------------------------------

inline void pin_thread_to_core(uint32_t core_id) {
#ifdef __linux__
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  int rc = pthread_setaffinity_np(...);
  if (rc != 0) {
    spdlog::warn("pthread_setaffinity_np failed for core {}: {}", core_id, rc);
  }
#else
  (void)core_id; // no-op on non-Linux
#endif
}

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

/// Конфигурация прогона.
struct ExecutorConfig {
  bool pin_threads = true;
  bool use_rdtsc = true;
  uint32_t metrics_sampling_ms = 200;
  uint64_t ops_per_ms_per_thread = 500'000;
  uint64_t base_seed = 12345;
};

/// Выполняет один прогон (run) сценария на заданном контейнере.
///
/// Горячий путь: worker_fn выполняется в потоке без аллокаций, Lua-вызовов,
/// cout, rand и т.д. Всё предвычислено.
inline RunResult execute_run(ContainerBase &container, const Scenario &scenario,
                             uint32_t thread_count, uint32_t run_index,
                             const ExecutorConfig &config, CsvWriter &csv) {
  spdlog::info("  Run {}: {} threads, scenario '{}'", run_index, thread_count,
               scenario.name);

  // ---- 1. Предвычисление операций (ДО старта замера) ----
  WorkloadConfig wc;
  wc.ops_per_ms_per_thread = config.ops_per_ms_per_thread;
  wc.base_seed = config.base_seed + run_index * 7919;

  auto all_ops = generate_scenario_ops(scenario, thread_count, wc);

  // ---- 2. Очистка контейнера ----
  container.clear();

  // ---- 3. Подготовка потоков ----
  std::vector<ThreadResult> results(thread_count);

  // Резервируем память для latencies ДО старта
  for (uint32_t t = 0; t < thread_count; ++t) {
    uint64_t total = 0;
    for (auto &phase_ops : all_ops) {
      total += phase_ops[t].size();
    }
    results[t].latencies.reserve(total);
  }

  // Барьер: все потоки стартуют одновременно
  std::barrier sync_start(thread_count + 1); // +1 для main thread
  std::barrier sync_end(thread_count + 1);

  // ---- 4. Запуск фонового сэмплера ----
  SystemSampler sampler(config.metrics_sampling_ms);

  // ---- 5. Запуск рабочих потоков ----
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (uint32_t tid = 0; tid < thread_count; ++tid) {
    workers.emplace_back([&, tid] {
      // Thread pinning
      if (config.pin_threads) {
        pin_thread_to_core(tid);
      }

      // libcds thread attachment (если применимо)
      // CdsThreadGuard cds_guard; // раскомментировать для libcds

      auto &res = results[tid];

      sync_start.arrive_and_wait(); // ждём старта

      // ---- ГОРЯЧИЙ ЦИКЛ ----
      // Нулевые аллокации, нулевые syscall'ы, нулевой Lua
      for (size_t pi = 0; pi < all_ops.size(); ++pi) {
        const auto &ops = all_ops[pi][tid];
        const auto phase_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(scenario.phases[pi].duration_ms);

        for (size_t i = 0; i < ops.size(); ++i) {
          // Проверяем дедлайн каждые 1024 операции (amortised)
          if ((i & 0x3FF) == 0) {
            if (std::chrono::steady_clock::now() >= phase_deadline)
              break;
          }

          const auto &op = ops[i];
          uint64_t t0 = rdtsc();

          switch (op.type) {
          case OpType::Insert:
            container.insert(op.key, op.key); // value = key
            break;
          case OpType::Find: {
            uint64_t val;
            container.find(op.key, val);
            break;
          }
          case OpType::Erase:
            container.erase(op.key);
            break;
          }

          uint64_t t1 = rdtsc();
          res.latencies.push_back(t1 - t0);
          ++res.ops_completed;
        }
      }

      sync_end.arrive_and_wait(); // сигнализируем о завершении
    });
  }

  // ---- 6. Старт замера ----
  sampler.start();
  auto wall_start = std::chrono::steady_clock::now();
  sync_start.arrive_and_wait(); // отпускаем потоки

  // Ждём завершения
  sync_end.arrive_and_wait();
  auto wall_end = std::chrono::steady_clock::now();
  sampler.stop();

  for (auto &w : workers)
    w.join();

  // ---- 7. Сбор результатов ----
  double duration_sec =
      std::chrono::duration<double>(wall_end - wall_start).count();

  uint64_t total_ops = 0;
  std::vector<double> all_latencies;
  for (auto &r : results) {
    total_ops += r.ops_completed;
    for (uint64_t lat : r.latencies) {
      all_latencies.push_back(static_cast<double>(lat));
    }
  }

  auto lat_stats = compute_stats(all_latencies);

  // Средние системные метрики
  double avg_cpu = 0.0, avg_rss = 0.0;
  const auto &samples = sampler.samples();
  if (!samples.empty()) {
    for (const auto &s : samples) {
      avg_cpu += s.cpu_total_pct;
      avg_rss += static_cast<double>(s.rss_bytes);
    }
    avg_cpu /= samples.size();
    avg_rss /= samples.size();
  }

  RunResult rr;
  rr.container_name = container.name();
  rr.scenario_name = scenario.name;
  rr.thread_count = thread_count;
  rr.run_index = run_index;
  rr.total_ops = total_ops;
  rr.duration_sec = duration_sec;
  rr.throughput_ops = static_cast<double>(total_ops) / duration_sec;
  rr.lat_p50 = lat_stats.p50;
  rr.lat_p95 = lat_stats.p95;
  rr.lat_p99 = lat_stats.p99;
  rr.lat_p999 = lat_stats.p999;
  rr.avg_cpu_pct = avg_cpu;
  rr.avg_rss_mb = avg_rss / (1024.0 * 1024.0);

  // Записываем в CSV
  csv.append_raw(rr);
  csv.write_system_metrics(rr.container_name, rr.scenario_name, rr.thread_count,
                           rr.run_index, samples);

  spdlog::info("    → {} ops in {:.2f}s = {:.0f} ops/sec, p99={:.0f}",
               total_ops, duration_sec, rr.throughput_ops, rr.lat_p99);

  return rr;
}

} // namespace bench
