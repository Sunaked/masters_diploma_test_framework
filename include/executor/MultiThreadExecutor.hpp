#pragma once
// ============================================================================
// MultiThreadExecutor.hpp — оркестрация многопоточного выполнения бенчмарка.
// ============================================================================

#include "containers/ContainerAdapter.hpp"
#include "metrics/ReservoirSampler.hpp"
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

inline uint64_t wall_clock_ns() {
  return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

struct alignas(64) ThreadResult {
  uint64_t ops_completed = 0;
  ReservoirSampler cycles_sampler; // rdtsc cycle latencies
  ReservoirSampler wall_sampler;   // wall-clock ns latencies

  // Per-phase counters for this thread
  struct PerPhase {
    uint64_t ops = 0;
    double duration_sec = 0.0;
    ReservoirSampler cycles;
    ReservoirSampler wall;
  };
  std::vector<PerPhase> phase_data;
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

enum class LatencyMode : uint8_t {
  Cycles = 0, // rdtsc only (original behavior)
  WallNs = 1, // wall-clock nanoseconds only
  Both = 2,   // record both (higher overhead but more info)
};

inline const char *to_string(LatencyMode m) {
  switch (m) {
  case LatencyMode::Cycles:
    return "cycles";
  case LatencyMode::WallNs:
    return "wall_ns";
  case LatencyMode::Both:
    return "both";
  }
  return "cycles";
}

struct ExecutorConfig {
  bool pin_threads = true;
  bool use_rdtsc = true; // legacy flag, now use latency_mode
  uint32_t metrics_sampling_ms = 200;
  uint64_t base_seed = 12345;
  bool dump_ops = true;

  // Overhead decomposition
  bool latency_off = false; // skip ALL latency recording (measures pure loop)
  bool sampler_off = false; // skip SystemSampler thread

  // Latency mode
  LatencyMode latency_mode = LatencyMode::Cycles;

  // Reservoir sampling capacity
  size_t reservoir_capacity = ReservoirSampler::kDefaultCapacity;

  // If warmup_auto_seconds > 0 and a phase has role=Warmup, it runs for at
  // least this many seconds (repeating the ops if needed). Otherwise, the
  // warmup phase just runs its declared ops once.
  double warmup_auto_seconds = 0.0;
};

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

  // ---- 2. Дамп trace в CSV ----
  if (config.dump_ops && run_index == 0) {
    csv.dump_all_ops(scenario.name, all_ops);
  }

  // ---- 3. Очистка контейнера ----
  container.clear();

  std::vector<PhaseRole> phase_roles;
  phase_roles.reserve(scenario.phases.size());
  for (const auto &ph : scenario.phases) {
    phase_roles.push_back(ph.role);
  }

  // ---- 4. Подготовка потоков ----
  std::vector<ThreadResult> results(thread_count);

  const size_t cap = config.reservoir_capacity;
  const bool record_cycles =
      !config.latency_off && (config.latency_mode == LatencyMode::Cycles ||
                              config.latency_mode == LatencyMode::Both);
  const bool record_wall =
      !config.latency_off && (config.latency_mode == LatencyMode::WallNs ||
                              config.latency_mode == LatencyMode::Both);

  for (uint32_t t = 0; t < thread_count; ++t) {
    results[t].cycles_sampler.reset(cap, config.base_seed + t * 31 + run_index);
    results[t].wall_sampler.reset(cap, config.base_seed + t * 37 + run_index);
    results[t].ops_completed = 0;

    // Per-phase data
    results[t].phase_data.resize(all_ops.size());
    for (size_t pi = 0; pi < all_ops.size(); ++pi) {
      results[t].phase_data[pi].cycles.reset(std::min(cap, (size_t)100'000),
                                             config.base_seed + t * 41 + pi);
      results[t].phase_data[pi].wall.reset(std::min(cap, (size_t)100'000),
                                           config.base_seed + t * 43 + pi);
    }
  }

  // BEFORE timing barrier, outside of measurement entirely]
  for (size_t pi = 0; pi < all_ops.size(); ++pi) {
    if (phase_roles[pi] != PhaseRole::Prefill)
      continue;

    spdlog::debug("Executing prefill phase {} (no timing, no sampling)", pi);

    // Run prefill single-threaded or multi-threaded without timing
    std::vector<std::thread> prefill_workers;
    prefill_workers.reserve(thread_count);
    for (uint32_t tid = 0; tid < thread_count; ++tid) {
      prefill_workers.emplace_back([&, tid, pi] {
        if (config.pin_threads)
          pin_thread_to_core(tid);
        const auto &ops = all_ops[pi][tid];
        for (size_t i = 0; i < ops.size(); ++i) {
          const auto &op = ops[i];
          switch (op.type) {
          case OpType::Insert:
            container.insert(op.key, op.key);
            break;
          case OpType::Find: {
            uint64_t v;
            container.find(op.key, v);
            break;
          }
          case OpType::Erase:
            container.erase(op.key);
            break;
          }
        }
        results[tid].phase_data[pi].ops = ops.size();
      });
    }
    for (auto &w : prefill_workers)
      w.join();
    spdlog::debug("Prefill phase {} complete", pi);
  }

  std::barrier sync_start(thread_count + 1);
  std::barrier sync_end(thread_count + 1);

  std::unique_ptr<SystemSampler> sampler;
  if (!config.sampler_off) {
    sampler = std::make_unique<SystemSampler>(config.metrics_sampling_ms);
  }

  // ---- 5. Рабочие потоки (ГОРЯЧИЙ ЦИКЛ) ----
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (uint32_t tid = 0; tid < thread_count; ++tid) {
    workers.emplace_back([&, tid] {
      if (config.pin_threads) {
        pin_thread_to_core(tid);
      }

      auto &res = results[tid];
      sync_start.arrive_and_wait();

      for (size_t pi = 0; pi < all_ops.size(); ++pi) {
        if (phase_roles[pi] == PhaseRole::Prefill)
          continue;

        const bool is_measure = (phase_roles[pi] == PhaseRole::Measure);
        const auto &ops = all_ops[pi][tid];
        auto phase_wall_start = std::chrono::steady_clock::now();

        // === Hot loop ===
        if (config.latency_off) {
          // Pure loop overhead measurement — no latency recording
          for (size_t i = 0; i < ops.size(); ++i) {
            const auto &op = ops[i];
            switch (op.type) {
            case OpType::Insert:
              container.insert(op.key, op.key);
              break;
            case OpType::Find: {
              uint64_t v;
              container.find(op.key, v);
              break;
            }
            case OpType::Erase:
              container.erase(op.key);
              break;
            }
          }
          res.phase_data[pi].ops = ops.size();
          if (is_measure)
            res.ops_completed += ops.size();

        } else if (record_cycles && !record_wall) {
          // Cycles-only mode (original hot path, minimal overhead)
          for (size_t i = 0; i < ops.size(); ++i) {
            const auto &op = ops[i];
            uint64_t t0 = rdtsc();
            switch (op.type) {
            case OpType::Insert:
              container.insert(op.key, op.key);
              break;
            case OpType::Find: {
              uint64_t v;
              container.find(op.key, v);
              break;
            }
            case OpType::Erase:
              container.erase(op.key);
              break;
            }
            uint64_t t1 = rdtsc();
            uint64_t lat = t1 - t0;
            // Reservoir sampling instead of vector push
            if (is_measure) {
              res.cycles_sampler.add(lat);
              res.phase_data[pi].cycles.add(lat);
            }
          }
          res.phase_data[pi].ops = ops.size();
          if (is_measure)
            res.ops_completed += ops.size();

        } else if (!record_cycles && record_wall) {
          // Wall-clock only mode
          for (size_t i = 0; i < ops.size(); ++i) {
            const auto &op = ops[i];
            auto w0 = std::chrono::steady_clock::now();
            switch (op.type) {
            case OpType::Insert:
              container.insert(op.key, op.key);
              break;
            case OpType::Find: {
              uint64_t v;
              container.find(op.key, v);
              break;
            }
            case OpType::Erase:
              container.erase(op.key);
              break;
            }
            auto w1 = std::chrono::steady_clock::now();
            uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0)
                    .count());
            if (is_measure) {
              res.wall_sampler.add(ns);
              res.phase_data[pi].wall.add(ns);
            }
          }
          res.phase_data[pi].ops = ops.size();
          if (is_measure)
            res.ops_completed += ops.size();

        } else {
          // Both cycles + wall-clock
          for (size_t i = 0; i < ops.size(); ++i) {
            const auto &op = ops[i];
            auto w0 = std::chrono::steady_clock::now();
            uint64_t c0 = rdtsc();
            switch (op.type) {
            case OpType::Insert:
              container.insert(op.key, op.key);
              break;
            case OpType::Find: {
              uint64_t v;
              container.find(op.key, v);
              break;
            }
            case OpType::Erase:
              container.erase(op.key);
              break;
            }
            uint64_t c1 = rdtsc();
            auto w1 = std::chrono::steady_clock::now();
            if (is_measure) {
              res.cycles_sampler.add(c1 - c0);
              res.phase_data[pi].cycles.add(c1 - c0);
              uint64_t ns = static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0)
                      .count());
              res.wall_sampler.add(ns);
              res.phase_data[pi].wall.add(ns);
            }
          }
          res.phase_data[pi].ops = ops.size();
          if (is_measure)
            res.ops_completed += ops.size();
        }

        auto phase_wall_end = std::chrono::steady_clock::now();
        res.phase_data[pi].duration_sec =
            std::chrono::duration<double>(phase_wall_end - phase_wall_start)
                .count();
      }

      sync_end.arrive_and_wait();
    });
  }

  // ---- 6. Старт замера ----
  if (sampler)
    sampler->start();
  auto wall_start = std::chrono::steady_clock::now();
  sync_start.arrive_and_wait();

  sync_end.arrive_and_wait();
  auto wall_end = std::chrono::steady_clock::now();
  if (sampler)
    sampler->stop();

  for (auto &w : workers)
    w.join();

  for (uint32_t t = 0; t < thread_count; ++t) {
    spdlog::debug(
        "Post-join: run={}, tid={}, ops={}, cyc_samples={}, wall_samples={}",
        run_index, t, results[t].ops_completed,
        results[t].cycles_sampler.size(), results[t].wall_sampler.size());
  }

  // ---- 7. Сбор результатов ----
  double duration_sec =
      std::chrono::duration<double>(wall_end - wall_start).count();

  uint64_t total_ops = 0;
  for (auto &r : results) {
    total_ops += r.ops_completed;
  }

  // stats from reservoirs
  // Compute cycle latency stats
  StatResult cyc_stats;
  {
    std::vector<ReservoirSampler> cyc_samplers;
    for (auto &r : results)
      cyc_samplers.push_back(std::move(r.cycles_sampler));
    if (!cyc_samplers.empty() && cyc_samplers[0].total_count > 0) {
      cyc_stats = compute_stats_from_reservoirs(cyc_samplers);
    }
  }

  // Compute wall-clock latency stats
  StatResult wall_stats;
  {
    std::vector<ReservoirSampler> wall_samplers;
    for (auto &r : results)
      wall_samplers.push_back(std::move(r.wall_sampler));
    if (!wall_samplers.empty() && wall_samplers[0].total_count > 0) {
      wall_stats = compute_stats_from_reservoirs(wall_samplers);
    }
  }

  // System metrics
  const auto &samples =
      sampler ? sampler->samples() : std::vector<SystemSample>{};

  double avg_cpu = std::numeric_limits<double>::quiet_NaN();
  double avg_rss = std::numeric_limits<double>::quiet_NaN();
  double avg_cs_per_sec = std::numeric_limits<double>::quiet_NaN();

  if (!samples.empty()) {
    double rss_sum = 0.0;
    for (const auto &s : samples) {
      rss_sum += static_cast<double>(s.rss_bytes);
    }
    avg_rss = rss_sum / static_cast<double>(samples.size());

    size_t cpu_begin = (samples.size() >= 2) ? 1 : 0;
    if (cpu_begin < samples.size()) {
      double cpu_sum = 0.0;
      size_t cpu_count = 0;
      for (size_t i = cpu_begin; i < samples.size(); ++i) {
        cpu_sum += samples[i].cpu_total_pct;
        ++cpu_count;
      }
      if (cpu_count > 0)
        avg_cpu = cpu_sum / static_cast<double>(cpu_count);
    }

    if (samples.size() >= 2 && duration_sec > 0.0) {
      const auto &first = samples.front();
      const auto &last = samples.back();
      uint64_t delta_cs =
          last.total_context_switches() - first.total_context_switches();
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
  rr.throughput_ops = (duration_sec > 0.0)
                          ? static_cast<double>(total_ops) / duration_sec
                          : 0.0;

  // Cycle latency (backward compat fields)
  rr.lat_p50 = cyc_stats.p50;
  rr.lat_p95 = cyc_stats.p95;
  rr.lat_p99 = cyc_stats.p99;
  rr.lat_p999 = cyc_stats.p999;

  // wall-clock latency
  rr.lat_wall_p50 = wall_stats.p50;
  rr.lat_wall_p95 = wall_stats.p95;
  rr.lat_wall_p99 = wall_stats.p99;
  rr.lat_wall_p999 = wall_stats.p999;
  rr.latency_mode = to_string(config.latency_mode);

  rr.avg_cpu_pct = avg_cpu;
  rr.avg_rss_mb = avg_rss / (1024.0 * 1024.0);
  rr.avg_cs_per_sec = avg_cs_per_sec;

  // Per-phase results
  for (size_t pi = 0; pi < all_ops.size(); ++pi) {
    PhaseResult pr;
    pr.phase_index = pi;
    pr.phase_role = to_string(phase_roles[pi]);

    // Aggregate per-phase across threads
    uint64_t phase_ops = 0;
    double max_phase_dur = 0.0;
    std::vector<ReservoirSampler> phase_cyc_samplers, phase_wall_samplers;

    for (uint32_t t = 0; t < thread_count; ++t) {
      auto &pd = results[t].phase_data[pi];
      phase_ops += pd.ops;
      max_phase_dur = std::max(max_phase_dur, pd.duration_sec);
      phase_cyc_samplers.push_back(std::move(pd.cycles));
      phase_wall_samplers.push_back(std::move(pd.wall));
    }

    pr.ops = phase_ops;
    pr.duration_sec = max_phase_dur;
    pr.throughput_ops = (max_phase_dur > 0.0)
                            ? static_cast<double>(phase_ops) / max_phase_dur
                            : 0.0;

    if (!phase_cyc_samplers.empty() && phase_cyc_samplers[0].total_count > 0) {
      auto ps = compute_stats_from_reservoirs(phase_cyc_samplers);
      pr.lat_cyc_p50 = ps.p50;
      pr.lat_cyc_p95 = ps.p95;
      pr.lat_cyc_p99 = ps.p99;
      pr.lat_cyc_p999 = ps.p999;
    }

    if (!phase_wall_samplers.empty() &&
        phase_wall_samplers[0].total_count > 0) {
      auto ps = compute_stats_from_reservoirs(phase_wall_samplers);
      pr.lat_wall_p50 = ps.p50;
      pr.lat_wall_p95 = ps.p95;
      pr.lat_wall_p99 = ps.p99;
      pr.lat_wall_p999 = ps.p999;
    }

    rr.phase_results.push_back(std::move(pr));
  }

  csv.append_raw(rr);
  csv.write_phase_results(rr);
  csv.write_system_metrics(rr.container_name, rr.scenario_name, rr.thread_count,
                           rr.run_index, samples);

  spdlog::debug("CSV written: run={}, ops={}, dur={:.6f}s, tput={:.3f}",
                run_index, rr.total_ops, rr.duration_sec, rr.throughput_ops);
  return rr;
}

} // namespace bench
