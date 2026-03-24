#pragma once
// ============================================================================
// MultiThreadExecutor.hpp — оркестрация многопоточного выполнения бенчмарка.
//
// Архитектура:
//   1. Предвычисление операций (WorkloadGenerator) — ДО старта замера
//   2. Дамп trace в CSV (ops/) — для воспроизводимости
//   3. Барьер → горячий цикл (нулевой Lua, нулевые аллокации) → барьер
//   4. Сбор результатов
// ============================================================================

#include "containers/ContainerAdapter.hpp"
#include "metrics/Statistics.hpp"
#include "metrics/SystemSampler.hpp"
#include "output/CsvWriter.hpp"
#include "workload/Phase.hpp"
#include "workload/WorkloadGenerator.hpp"

#include <sol/sol.hpp>
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
// rdtsc
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
inline uint64_t rdtsc() {
  uint32_t lo, hi;
  asm volatile("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
  return (static_cast<uint64_t>(hi) << 32) | lo;
}
#else
inline uint64_t rdtsc() {
  return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}
#endif

// ---------------------------------------------------------------------------
// Per-thread result
// ---------------------------------------------------------------------------

struct alignas(64) ThreadResult {
  uint64_t ops_completed = 0;
  std::vector<uint64_t> latencies;
};

// ---------------------------------------------------------------------------
// Pin thread to core
// ---------------------------------------------------------------------------

inline void pin_thread_to_core(uint32_t core_id) {
#ifdef __linux__
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    spdlog::warn("pthread_setaffinity_np failed for core {}: {}", core_id, rc);
  }
#else
  (void)core_id;
#endif
}

// ---------------------------------------------------------------------------
// Executor config
// ---------------------------------------------------------------------------

struct ExecutorConfig {
  bool pin_threads = true;
  bool use_rdtsc = true;
  uint32_t metrics_sampling_ms = 200;
  uint64_t base_seed = 12345;
  bool dump_ops = true; ///< дампить trace в ops/ директорию
};

// ---------------------------------------------------------------------------
// execute_run
// ---------------------------------------------------------------------------

inline RunResult execute_run(const std::string &container_name,
                             ContainerBase &container, const Scenario &scenario,
                             uint32_t thread_count, uint32_t run_index,
                             const ExecutorConfig &config, CsvWriter &csv,
                             sol::state &lua) {
  spdlog::debug("execute_run begin: container='{}', scenario='{}', threads={}, "
                "run_index={}",
                container_name, scenario.name, thread_count, run_index);

  // ---- 1. Предвычисление операций ----
  WorkloadConfig wc;
  wc.base_seed = config.base_seed + run_index * 7919;

  auto all_ops = generate_scenario_ops(scenario, thread_count, wc, lua);
  spdlog::debug("Workload generated: phases={}, thread_count={}",
                all_ops.size(), thread_count);

  // ---- 2. Дамп trace в CSV (только для первого run каждого scenario×threads,
  //         чтобы не забить диск при 100+ прогонах) ----
  if (config.dump_ops && run_index == 0) {
    csv.dump_all_ops(scenario.name, all_ops);
  }

  // ---- 3. Очистка контейнера ----
  container.clear();

  // ---- 4. Подготовка потоков ----
  std::vector<ThreadResult> results(thread_count);

  for (uint32_t t = 0; t < thread_count; ++t) {
    uint64_t total = 0;
    for (const auto &phase_ops : all_ops) {
      total += phase_ops[t].size();
    }
    spdlog::debug("Expected ops: run={}, thread={}, expected_ops={}", run_index,
                  t, total);
    results[t].latencies.resize(total);
    results[t].ops_completed = 0;
  }

  std::barrier sync_start(thread_count + 1);
  std::barrier sync_end(thread_count + 1);

  SystemSampler sampler(
      config.metrics_sampling_ms); // using metrics_sampling_ms from config

  // ---- 5. Рабочие потоки (ГОРЯЧИЙ ЦИКЛ) ----
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (uint32_t tid = 0; tid < thread_count; ++tid) {
    workers.emplace_back([&, tid] {
      size_t lat_idx = 0;

      if (config.pin_threads) {
        pin_thread_to_core(tid);
      }

      auto &res = results[tid];
      sync_start.arrive_and_wait();

      for (size_t pi = 0; pi < all_ops.size(); ++pi) {
        const auto &ops = all_ops[pi][tid];

        for (size_t i = 0; i < ops.size(); ++i) {
          const auto &op = ops[i];
          uint64_t t0 = rdtsc();

          switch (op.type) {
          case OpType::Insert:
            container.insert(op.key, op.key);
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
          if (lat_idx >= res.latencies.size()) {
            throw std::runtime_error(
                "latency buffer overflow: tid=" + std::to_string(tid) +
                ", lat_idx=" + std::to_string(lat_idx) +
                ", size=" + std::to_string(res.latencies.size()));
          }
          res.latencies[lat_idx++] = t1 - t0;
          ++res.ops_completed;
        }
      }

      if (lat_idx != res.ops_completed) {
        throw std::runtime_error("lat_idx != ops_completed: tid=" +
                                 std::to_string(tid));
      }
      res.latencies.resize(lat_idx);
      sync_end.arrive_and_wait();
    });
  }

  // ---- 6. Старт замера ----
  sampler.start();
  auto wall_start = std::chrono::steady_clock::now();
  sync_start.arrive_and_wait();

  sync_end.arrive_and_wait();
  auto wall_end = std::chrono::steady_clock::now();
  sampler.stop();

  for (auto &w : workers)
    w.join();

  for (uint32_t t = 0; t < thread_count; ++t) {
    spdlog::debug("Post-join: run={}, tid={}, ops={}, lat_buf={}", run_index, t,
                  results[t].ops_completed, results[t].latencies.size());
  }

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

  const auto &samples = sampler.samples();

  double avg_cpu = std::numeric_limits<double>::quiet_NaN();
  double avg_rss = std::numeric_limits<double>::quiet_NaN();
  double avg_cs_per_sec = std::numeric_limits<double>::quiet_NaN();

  if (!samples.empty()) {
    double rss_sum = 0.0;
    for (const auto &s : samples) {
      rss_sum += static_cast<double>(s.rss_bytes);
    }
    avg_rss = rss_sum / static_cast<double>(samples.size());

    // CPU: первую baseline sample не учитываем, если есть хотя бы 2 sample
    size_t cpu_begin = 0;
    if (samples.size() >= 2) {
      cpu_begin = 1;
    }

    if (cpu_begin < samples.size()) {
      double cpu_sum = 0.0;
      size_t cpu_count = 0;
      for (size_t i = cpu_begin; i < samples.size(); ++i) {
        cpu_sum += samples[i].cpu_total_pct;
        ++cpu_count;
      }
      if (cpu_count > 0) {
        avg_cpu = cpu_sum / static_cast<double>(cpu_count);
      }
    }

    if (samples.size() >= 2 && duration_sec > 0.0) {
      const auto &first = samples.front();
      const auto &last = samples.back();
      uint64_t first_cs = first.total_context_switches();
      uint64_t last_cs = last.total_context_switches();
      uint64_t delta_cs = (last_cs >= first_cs) ? (last_cs - first_cs) : 0ULL;
      avg_cs_per_sec = static_cast<double>(delta_cs) / duration_sec;
    }
  }

  RunResult rr;
  rr.container_name = container_name;
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
  rr.avg_cs_per_sec = avg_cs_per_sec;

  csv.append_raw(rr);
  csv.write_system_metrics(rr.container_name, rr.scenario_name, rr.thread_count,
                           rr.run_index, samples);

  spdlog::debug("CSV written: run={}, ops={}, dur={:.6f}s, tput={:.3f}",
                run_index, rr.total_ops, rr.duration_sec, rr.throughput_ops);
  return rr;
}

} // namespace bench
