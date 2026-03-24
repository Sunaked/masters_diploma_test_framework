#pragma once
// ============================================================================
// CsvWriter.hpp — вывод результатов бенчмарка в CSV.
//
// Структура вывода (один эксперимент = одна папка):
//
//   results/run_25_03_24_143022/
//     config_global.csv          — глобальные параметры
//     config_containers.csv      — список контейнеров с бюджетами времени
//     config_scenarios.csv       — описание всех фаз всех сценариев
//     ops/                       — предвычисленные trace для каждого (scenario,
//     phase, thread)
//       <scenario>_phase<N>_thread<M>.csv
//     raw_results.csv            — каждая строка = один прогон
//     summary.csv                — агрегированные статистики
//     system_metrics.csv         — системные метрики с временными метками
//
// Формат CSV для совместимости с pandas, R, gnuplot, Excel.
// ============================================================================

#include "metrics/Statistics.hpp"
#include "metrics/SystemSampler.hpp"
#include "workload/Phase.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// RunResult / SummaryEntry (без изменений)
// ---------------------------------------------------------------------------

struct RunResult {
  std::string container_name;
  std::string scenario_name;
  uint32_t thread_count = 0;
  uint32_t run_index = 0;
  double avg_cs_per_sec = std::numeric_limits<double>::quiet_NaN();

  uint64_t total_ops = 0;
  double duration_sec = 0.0;
  double throughput_ops = 0.0;

  double lat_p50 = 0.0;
  double lat_p95 = 0.0;
  double lat_p99 = 0.0;
  double lat_p999 = 0.0;

  double avg_cpu_pct = 0.0;
  double avg_rss_mb = 0.0;
};

struct SummaryEntry {
  std::string container_name;
  std::string scenario_name;
  uint32_t thread_count = 0;
  uint32_t num_runs = 0;

  StatResult throughput_stats;
  StatResult lat_p50_stats;
  StatResult lat_p99_stats;
};

// ---------------------------------------------------------------------------
// CsvWriter
// ---------------------------------------------------------------------------

class CsvWriter {
public:
  /// Создаёт директорию results/run_YY_MM_DD_HHMMSS/ .
  explicit CsvWriter(const std::string &base_dir = "results") {
    run_dir_ = base_dir + "/" + make_run_dirname();
    ops_dir_ = run_dir_ + "/ops";

    std::filesystem::create_directories(run_dir_);
    std::filesystem::create_directories(ops_dir_);

    raw_path_ = run_dir_ + "/raw_results.csv";
    summary_path_ = run_dir_ + "/summary.csv";
    system_path_ = run_dir_ + "/system_metrics.csv";

    spdlog::info("Output directory: {}", run_dir_);
  }

  // =====================================================================
  // Конфигурация (входные параметры)
  // =====================================================================

  /// Дампит глобальные параметры в config_global.csv.
  void dump_config_global(double target_total_time_minutes,
                          uint32_t base_runs_per_config,
                          uint32_t min_runs_per_config,
                          uint32_t metrics_sampling_ms, bool pin_threads,
                          bool use_rdtsc_for_latency,
                          const std::vector<uint32_t> &threads_list) {
    {
      std::ofstream f(run_dir_ + "/config_global.csv");
      f << "parameter,value\n"
        << "target_total_time_minutes," << target_total_time_minutes << "\n"
        << "base_runs_per_config," << base_runs_per_config << "\n"
        << "min_runs_per_config," << min_runs_per_config << "\n"
        << "metrics_sampling_ms," << metrics_sampling_ms << "\n"
        << "pin_threads," << (pin_threads ? "true" : "false") << "\n"
        << "use_rdtsc_for_latency,"
        << (use_rdtsc_for_latency ? "true" : "false") << "\n";

      // threads_list как отдельная строка
      f << "threads_list,\"";
      for (size_t i = 0; i < threads_list.size(); ++i) {
        if (i > 0)
          f << ";";
        f << threads_list[i];
      }
      f << "\"\n";
    }
    spdlog::debug("Dumped config_global.csv");
  }

  /// Дампит список контейнеров в config_containers.csv.
  void dump_config_containers(const std::vector<std::string> &names,
                              const std::vector<std::string> &types,
                              const std::vector<double> &target_minutes) {
    std::ofstream f(run_dir_ + "/config_containers.csv");
    f << "name,type,target_time_minutes\n";
    for (size_t i = 0; i < names.size(); ++i) {
      f << names[i] << "," << types[i] << "," << target_minutes[i] << "\n";
    }
    spdlog::debug("Dumped config_containers.csv ({} containers)", names.size());
  }

  /// Дампит описание всех сценариев и фаз в config_scenarios.csv.
  void dump_config_scenarios(const std::vector<Scenario> &scenarios) {
    std::ofstream f(run_dir_ + "/config_scenarios.csv");
    f << "scenario,phase_index,mode,"
         "ops_per_thread,insert_ratio,find_ratio,erase_ratio,"
         "key_dist,alpha,key_range,"
         "lua_generator,"
         "plan_entries_count\n";

    for (const auto &sc : scenarios) {
      for (size_t pi = 0; pi < sc.phases.size(); ++pi) {
        const auto &ph = sc.phases[pi];

        f << sc.name << "," << pi << ",";

        switch (ph.mode) {
        case PhaseMode::Probabilistic:
          f << "probabilistic," << ph.ops_per_thread << "," << ph.insert_ratio
            << "," << ph.find_ratio << "," << ph.erase_ratio << ",";
          switch (ph.key_dist) {
          case KeyDist::Uniform:
            f << "uniform,";
            break;
          case KeyDist::Zipfian:
            f << "zipfian,";
            break;
          case KeyDist::Sequential:
            f << "sequential,";
            break;
          }
          f << ph.alpha << "," << ph.key_range << ","
            << ","   // lua_generator empty
            << "\n"; // plan_entries_count empty
          break;

        case PhaseMode::ScriptedStep:
          f << "scripted_step," << ph.ops_per_thread << ","
            << ",,,"; // ratios empty
          f << ","    // key_dist empty
            << ph.alpha << "," << ph.key_range << "," << ph.lua_generator_name
            << ","
            << "\n";
          break;

        case PhaseMode::ScriptedPlan:
          f << "scripted_plan," << ph.ops_per_thread << ","
            << ",,,,,,,"; // ratios/dist/alpha/range/lua empty
          f << ph.plan_entries.size() << "\n";

          // Дампим plan entries как дополнительные строки
          for (size_t ei = 0; ei < ph.plan_entries.size(); ++ei) {
            const auto &pe = ph.plan_entries[ei];
            // Переиспользуем формат, но с пометкой в scenario
            // (отдельный CSV был бы чище, но держим всё в одном файле)
          }
          break;
        }
      }
    }

    // Отдельный CSV для plan entries (если есть)
    dump_plan_entries(scenarios);

    spdlog::debug("Dumped config_scenarios.csv ({} scenarios)",
                  scenarios.size());
  }

  // =====================================================================
  // Операции (предвычисленный trace)
  // =====================================================================

  /// Дампит предвычисленные операции для одного (scenario, phase, thread).
  /// Вызывается после generate_scenario_ops, до горячего цикла.
  ///
  /// Файл: ops/<scenario>_phase<N>_thread<M>.csv
  /// Формат: op_index,op_type,key
  void dump_ops_trace(const std::string &scenario_name, size_t phase_index,
                      uint32_t thread_id, const std::vector<Op> &ops) {
    std::string filename = ops_dir_ + "/" + scenario_name + "_phase" +
                           std::to_string(phase_index) + "_thread" +
                           std::to_string(thread_id) + ".csv";

    std::ofstream f(filename);
    f << "op_index,op_type,key\n";
    for (size_t i = 0; i < ops.size(); ++i) {
      f << i << "," << to_string(ops[i].type) << "," << ops[i].key << "\n";
    }
  }

  /// Дампит все trace для всего сценария (все фазы, все потоки).
  /// all_ops[phase_index][thread_id] = vector<Op>
  void dump_all_ops(const std::string &scenario_name,
                    const std::vector<std::vector<std::vector<Op>>> &all_ops) {
    for (size_t pi = 0; pi < all_ops.size(); ++pi) {
      for (size_t tid = 0; tid < all_ops[pi].size(); ++tid) {
        dump_ops_trace(scenario_name, pi, static_cast<uint32_t>(tid),
                       all_ops[pi][tid]);
      }
    }
    spdlog::debug("Dumped ops trace: scenario='{}', phases={}, threads={}",
                  scenario_name, all_ops.size(),
                  all_ops.empty() ? 0 : all_ops[0].size());
  }

  // =====================================================================
  // Метрики (выходные данные)
  // =====================================================================

  void write_raw_header() {
    std::ofstream f(raw_path_, std::ios::trunc);
    f << "container,scenario,threads,run,"
         "total_ops,duration_sec,throughput_ops_sec,"
         "lat_p50_cycles,lat_p95_cycles,lat_p99_cycles,lat_p999_cycles,"
         "avg_cpu_pct,avg_rss_mb,avg_cs_per_sec\n";
  }

  void append_raw(const RunResult &r) {
    std::ofstream f(raw_path_, std::ios::app);
    f << r.container_name << "," << r.scenario_name << "," << r.thread_count
      << "," << r.run_index << "," << r.total_ops << "," << r.duration_sec
      << "," << r.throughput_ops << "," << r.lat_p50 << "," << r.lat_p95 << ","
      << r.lat_p99 << "," << r.lat_p999 << "," << r.avg_cpu_pct << ","
      << r.avg_rss_mb << "," << r.avg_cs_per_sec << "\n";
  }

  void write_summary(const std::vector<SummaryEntry> &entries) {
    std::ofstream f(summary_path_, std::ios::trunc);
    f << "container,scenario,threads,num_runs,"
         "throughput_mean,throughput_stddev,throughput_ci95_lo,throughput_ci95_"
         "hi,"
         "throughput_min,throughput_max,"
         "lat_p50_mean,lat_p50_stddev,"
         "lat_p99_mean,lat_p99_stddev\n";

    for (const auto &e : entries) {
      f << e.container_name << "," << e.scenario_name << "," << e.thread_count
        << "," << e.num_runs << "," << e.throughput_stats.mean << ","
        << e.throughput_stats.stddev << "," << e.throughput_stats.ci95_lo << ","
        << e.throughput_stats.ci95_hi << "," << e.throughput_stats.min_val
        << "," << e.throughput_stats.max_val << "," << e.lat_p50_stats.mean
        << "," << e.lat_p50_stats.stddev << "," << e.lat_p99_stats.mean << ","
        << e.lat_p99_stats.stddev << "\n";
    }
  }

  void write_system_metrics(const std::string &container,
                            const std::string &scenario, uint32_t threads,
                            uint32_t run,
                            const std::vector<SystemSample> &samples) {
    std::ios_base::openmode mode =
        system_header_written_ ? std::ios::app : std::ios::trunc;
    std::ofstream f(system_path_, mode);

    if (!system_header_written_) {
      f << "container,scenario,threads,run,"
           "timestamp_us,cpu_pct,rss_bytes,"
           "vol_cs,invol_cs,page_faults,llc_misses\n";
      system_header_written_ = true;
    }

    for (const auto &s : samples) {
      f << container << "," << scenario << "," << threads << "," << run << ","
        << s.timestamp_us << "," << s.cpu_total_pct << "," << s.rss_bytes << ","
        << s.voluntary_cs << "," << s.involuntary_cs << "," << s.page_faults
        << "," << s.llc_misses << "\n";
    }
  }

  // =====================================================================
  // Accessors
  // =====================================================================

  const std::string &run_dir() const { return run_dir_; }
  const std::string &raw_path() const { return raw_path_; }
  const std::string &summary_path() const { return summary_path_; }
  const std::string &system_path() const { return system_path_; }
  const std::string &ops_dir() const { return ops_dir_; }

private:
  std::string run_dir_;
  std::string ops_dir_;
  std::string raw_path_;
  std::string summary_path_;
  std::string system_path_;
  bool system_header_written_ = false;

  /// Генерирует имя директории: run_YY_MM_DD_HHMMSS
  static std::string make_run_dirname() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream ss;
    ss << "run_" << std::setfill('0') << std::setw(2) << (tm.tm_year % 100)
       << "_" << std::setw(2) << (tm.tm_mon + 1) << "_" << std::setw(2)
       << tm.tm_mday << "_" << std::setw(2) << tm.tm_hour << std::setw(2)
       << tm.tm_min << std::setw(2) << tm.tm_sec;
    return ss.str();
  }

  /// Дампит plan entries для ScriptedPlan фаз.
  void dump_plan_entries(const std::vector<Scenario> &scenarios) {
    bool has_plans = false;
    for (const auto &sc : scenarios) {
      for (const auto &ph : sc.phases) {
        if (ph.mode == PhaseMode::ScriptedPlan && !ph.plan_entries.empty()) {
          has_plans = true;
          break;
        }
      }
      if (has_plans)
        break;
    }
    if (!has_plans)
      return;

    std::ofstream f(run_dir_ + "/config_plan_entries.csv");
    f << "scenario,phase_index,entry_index,"
         "count,op,key_mode,start,fixed_key,key_range,alpha\n";

    for (const auto &sc : scenarios) {
      for (size_t pi = 0; pi < sc.phases.size(); ++pi) {
        const auto &ph = sc.phases[pi];
        if (ph.mode != PhaseMode::ScriptedPlan)
          continue;

        for (size_t ei = 0; ei < ph.plan_entries.size(); ++ei) {
          const auto &pe = ph.plan_entries[ei];
          f << sc.name << "," << pi << "," << ei << "," << pe.count << ","
            << to_string(pe.op) << ",";

          switch (pe.key_mode) {
          case PlanKeyMode::Sequential:
            f << "sequential,";
            break;
          case PlanKeyMode::Fixed:
            f << "fixed,";
            break;
          case PlanKeyMode::Uniform:
            f << "uniform,";
            break;
          case PlanKeyMode::Zipfian:
            f << "zipfian,";
            break;
          }

          f << pe.start << "," << pe.fixed_key << "," << pe.key_range << ","
            << pe.alpha << "\n";
        }
      }
    }
  }
};

} // namespace bench
