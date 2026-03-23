// ============================================================================
// main.cpp — точка входа бенчмарк-фреймворка.
//
// Последовательность:
//   1. Загрузка конфигурации из Lua
//   2. Расчёт бюджета времени (target_time_minutes → runs_per_config)
//   3. Создание контейнеров (фабрика по type из Lua)
//   4. Цикл: для каждого контейнера × сценарий × threads × run → execute_run
//   5. Агрегация → summary.csv
//
// Установка:
//   git clone <repo>
//   ./setup.sh          # устанавливает зависимости через vcpkg
//   cmake -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build -j$(nproc)
//   ./build/bench [config/default.lua]
// ============================================================================

#include "containers/ContainerAdapter.hpp"
// #include "containers/CDS_Adapters.hpp"
// #include "containers/CK_HT_Adapter.hpp"
#include "containers/CustomAdapter.hpp"
#include "executor/MultiThreadExecutor.hpp"
#include "metrics/Statistics.hpp"
#include "output/CsvWriter.hpp"
#include "workload/Phase.hpp"

#include <sol/sol.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace bench;

// ============================================================================
// Конфигурационные структуры (заполняются из Lua)
// ============================================================================

struct GlobalConfig {
  double target_total_time_minutes = 240;
  uint32_t base_runs_per_config = 5;
  uint32_t min_runs_per_config = 3;
  uint32_t metrics_sampling_ms = 200;
  bool pin_threads = true;
  bool use_rdtsc_for_latency = true;
  uint64_t ops_per_ms_per_thread = 500;
};

struct ContainerConfig {
  std::string name;
  std::string type;
  double target_time_minutes = 60;
};

// ============================================================================
// Загрузка конфигурации из Lua
// ============================================================================

GlobalConfig load_global(sol::state &lua) {
  GlobalConfig g;
  sol::table tbl = lua["global"];
  g.target_total_time_minutes = tbl.get_or("target_total_time_minutes", 240.0);
  g.base_runs_per_config = tbl.get_or("base_runs_per_config", 5u);
  g.min_runs_per_config = tbl.get_or("min_runs_per_config", 3u);
  g.metrics_sampling_ms = tbl.get_or("metrics_sampling_ms", 200u);
  g.pin_threads = tbl.get_or("pin_threads", true);
  g.use_rdtsc_for_latency = tbl.get_or("use_rdtsc_for_latency", true);
  g.ops_per_ms_per_thread = tbl.get_or("ops_per_ms_per_thread", 500);
  return g;
}

std::vector<uint32_t> load_threads_list(sol::state &lua) {
  std::vector<uint32_t> list;
  sol::table tbl = lua["threads_list"];
  for (size_t i = 1; i <= tbl.size(); ++i) {
    list.push_back(tbl[i].get<uint32_t>());
  }
  return list;
}

std::vector<ContainerConfig> load_containers(sol::state &lua) {
  std::vector<ContainerConfig> result;
  sol::table tbl = lua["containers"];
  for (size_t i = 1; i <= tbl.size(); ++i) {
    sol::table c = tbl[i];
    ContainerConfig cc;
    cc.name = c["name"].get<std::string>();
    cc.type = c["type"].get<std::string>();
    cc.target_time_minutes = c.get_or("target_time_minutes", 60.0);
    result.push_back(std::move(cc));
  }
  return result;
}

std::vector<Scenario> load_scenarios(sol::state &lua) {
  std::vector<Scenario> result;
  sol::table tbl = lua["scenarios"];
  for (size_t i = 1; i <= tbl.size(); ++i) {
    sol::table s = tbl[i];
    Scenario sc;
    sc.name = s["name"].get<std::string>();

    sol::table phases = s["phases"];
    for (size_t j = 1; j <= phases.size(); ++j) {
      sol::table p = phases[j];
      Phase ph;
      ph.duration_ms = p.get_or("duration_ms", 10000ULL);
      ph.insert_ratio = p.get_or("insert", 0.0);
      ph.find_ratio = p.get_or("find", 1.0);
      ph.erase_ratio = p.get_or("erase", 0.0);
      ph.key_range = p.get_or("key_range", 1000000ULL);
      ph.alpha = p.get_or("alpha", 0.99);

      std::string dist = p.get_or<std::string>("key_dist", "uniform");
      if (dist == "zipfian")
        ph.key_dist = KeyDist::Zipfian;
      else if (dist == "sequential")
        ph.key_dist = KeyDist::Sequential;
      else
        ph.key_dist = KeyDist::Uniform;

      sc.phases.push_back(ph);
    }
    result.push_back(std::move(sc));
  }
  return result;
}

// ============================================================================
// Фабрика контейнеров
// ============================================================================

std::unique_ptr<ContainerBase> create_container(const std::string &type) {
  // Мост CRTP → virtual для type-erased хранения.
  // В горячем цикле вызовы идут через virtual (ContainerBase),
  // но overhead < 5 нс на вызов — приемлемо для прототипа.
  // Для production: шаблонный executor по каждому типу контейнера.

  // if (type == "cds::SplitListMap") {
  //   return std::make_unique<ContainerBridge<SplitListMapAdapter>>();
  // }
  // if (type == "cds::FeldmanHashMap") {
  //   return std::make_unique<ContainerBridge<FeldmanHashMapAdapter>>();
  // }
  // if (type == "ck_ht") {
  //   return std::make_unique<ContainerBridge<CK_HT_Adapter>>();
  // }
  if (type == "custom") {
    return std::make_unique<ContainerBridge<ShardedHashMap<256>>>();
  }

  spdlog::error("Unknown container type: '{}'", type);
  return nullptr;
}

// ============================================================================
// Логика распределения времени
//
// Алгоритм:
//   1. Суммируем target_time_minutes всех контейнеров → T_total
//   2. Для каждого контейнера:
//      a. Вычисляем длительность одного прогона:
//         run_duration = Σ phase.duration_ms для всех фаз всех сценариев
//      b. Количество конфигураций = |scenarios| × |threads_list|
//      c. Доступное время = target_time_minutes контейнера
//      d. Максимум прогонов = доступное_время / run_duration
//      e. runs_per_config = max(min_runs, max_прогонов / кол-во_конфигураций)
//   3. Если runs_per_config < min_runs → предупреждение
//   4. Приоритет: равномерность по threads и сценариям
// ============================================================================

struct RunPlan {
  std::string container_name;
  std::string container_type;
  struct Config {
    std::string scenario_name;
    uint32_t thread_count;
    uint32_t num_runs;
  };
  std::vector<Config> configs;
};

std::vector<RunPlan>
compute_run_plans(const std::vector<ContainerConfig> &containers,
                  const std::vector<Scenario> &scenarios,
                  const std::vector<uint32_t> &threads_list,
                  const GlobalConfig &global) {
  std::vector<RunPlan> plans;

  for (const auto &cc : containers) {
    RunPlan plan;
    plan.container_name = cc.name;
    plan.container_type = cc.type;

    // Длительность одного полного прогона (один сценарий, все фазы)
    // в минутах. Берём максимум среди сценариев для оценки.
    double max_scenario_duration_min = 0.0;
    double total_scenario_duration_min = 0.0;
    for (const auto &sc : scenarios) {
      double dur_ms = 0.0;
      for (const auto &ph : sc.phases) {
        dur_ms += static_cast<double>(ph.duration_ms);
      }
      double dur_min = dur_ms / 60000.0;
      max_scenario_duration_min = std::max(max_scenario_duration_min, dur_min);
      total_scenario_duration_min += dur_min;
    }

    // Количество конфигураций = |scenarios| × |threads_list|
    uint32_t num_configs =
        static_cast<uint32_t>(scenarios.size() * threads_list.size());

    // Доступное время в минутах
    double available_min = cc.target_time_minutes;

    // Максимальное количество прогонов (общее)
    // Один прогон = один сценарий × один threads_count
    // Средняя длительность прогона ≈ total_scenario_duration / |scenarios|
    double avg_run_min = total_scenario_duration_min / scenarios.size();
    if (avg_run_min <= 0)
      avg_run_min = 0.5; // fallback: 30 сек

    uint32_t max_total_runs =
        static_cast<uint32_t>(available_min / avg_run_min);

    // Распределяем равномерно по конфигурациям
    uint32_t runs_per_config =
        (num_configs > 0)
            ? std::max(global.min_runs_per_config, max_total_runs / num_configs)
            : global.base_runs_per_config;

    // Ограничиваем сверху base_runs_per_config (если времени хватает)
    runs_per_config = std::min(runs_per_config, global.base_runs_per_config);
    // Гарантируем минимум
    runs_per_config = std::max(runs_per_config, global.min_runs_per_config);

    // Проверка: хватит ли времени для min_runs × all configs
    double estimated_total_min = runs_per_config * num_configs * avg_run_min;
    if (estimated_total_min > available_min * 1.2) {
      spdlog::warn("Container '{}': estimated time {:.1f} min exceeds budget "
                   "{:.1f} min. "
                   "Reducing runs to fit.",
                   cc.name, estimated_total_min, available_min);
      // Принудительно уменьшаем, сохраняя минимум
      while (runs_per_config > global.min_runs_per_config &&
             runs_per_config * num_configs * avg_run_min > available_min) {
        --runs_per_config;
      }
    }

    // Генерируем конфигурации
    for (const auto &sc : scenarios) {
      for (uint32_t thr : threads_list) {
        plan.configs.push_back({sc.name, thr, runs_per_config});
      }
    }

    spdlog::info("Container '{}': {} configs × {} runs = {} total runs "
                 "(budget: {:.1f} min, est: {:.1f} min)",
                 cc.name, num_configs, runs_per_config,
                 num_configs * runs_per_config, available_min,
                 runs_per_config * num_configs * avg_run_min);

    plans.push_back(std::move(plan));
  }

  return plans;
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char *argv[]) {
  // ---- Логирование ----
  auto console = spdlog::stdout_color_mt("bench");
  spdlog::set_default_logger(console);
  spdlog::set_level(spdlog::level::info);
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  // ---- Инициализация libcds (глобальная, один раз) ----
  // cds_global_init();

  // ---- Загрузка конфигурации из Lua ----
  std::string config_path = (argc > 1) ? argv[1] : "config/default.lua";
  spdlog::info("Loading configuration from: {}", config_path);

  sol::state lua;
  lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table);

  try {
    lua.script_file(config_path);
  } catch (const sol::error &e) {
    spdlog::error("Lua config error: {}", e.what());
    return 1;
  }

  auto global_cfg = load_global(lua);
  auto threads_list = load_threads_list(lua);
  auto container_cfg = load_containers(lua);
  auto scenarios = load_scenarios(lua);

  spdlog::info(
      "Configuration loaded: {} containers, {} scenarios, {} thread counts",
      container_cfg.size(), scenarios.size(), threads_list.size());

  // ---- С этого момента Lua больше НЕ вызывается ----
  // Все параметры скопированы в C++ структуры.

  // ---- Расчёт бюджета времени ----
  auto plans =
      compute_run_plans(container_cfg, scenarios, threads_list, global_cfg);

  // ---- Подготовка вывода ----
  CsvWriter csv("results");
  csv.write_raw_header();

  // ---- Основной цикл бенчмарка ----
  // Собираем все RunResult для финальной агрегации
  std::map<std::string, std::vector<RunResult>> results_by_key;
  // key = "container|scenario|threads"

  ExecutorConfig exec_cfg;
  exec_cfg.pin_threads = global_cfg.pin_threads;
  exec_cfg.use_rdtsc = global_cfg.use_rdtsc_for_latency;
  exec_cfg.metrics_sampling_ms = global_cfg.metrics_sampling_ms;
  exec_cfg.ops_per_ms_per_thread = global_cfg.ops_per_ms_per_thread;

  for (const auto &plan : plans) {
    spdlog::info("=== Container: {} (type: {}) ===", plan.container_name,
                 plan.container_type);

    auto container = create_container(plan.container_type);
    if (!container) {
      spdlog::error("Skipping container '{}'", plan.container_name);
      continue;
    }

    for (const auto &cfg : plan.configs) {
      // Находим сценарий по имени
      const Scenario *scenario = nullptr;
      for (const auto &sc : scenarios) {
        if (sc.name == cfg.scenario_name) {
          scenario = &sc;
          break;
        }
      }
      if (!scenario)
        continue;

      spdlog::info("  Config: scenario='{}', threads={}, runs={}",
                   cfg.scenario_name, cfg.thread_count, cfg.num_runs);

      for (uint32_t run = 0; run < cfg.num_runs; ++run) {
        auto rr = execute_run(*container, *scenario, cfg.thread_count, run,
                              exec_cfg, csv);

        std::string key = plan.container_name + "|" + cfg.scenario_name + "|" +
                          std::to_string(cfg.thread_count);
        results_by_key[key].push_back(std::move(rr));
      }
    }
  }

  // ---- Агрегация: summary.csv ----
  spdlog::info("Computing summary statistics...");

  std::vector<SummaryEntry> summary;
  for (auto &[key, runs] : results_by_key) {
    if (runs.empty())
      continue;

    SummaryEntry entry;
    entry.container_name = runs[0].container_name;
    entry.scenario_name = runs[0].scenario_name;
    entry.thread_count = runs[0].thread_count;
    entry.num_runs = static_cast<uint32_t>(runs.size());

    std::vector<double> throughputs, lat_p50s, lat_p99s;
    for (const auto &r : runs) {
      throughputs.push_back(r.throughput_ops);
      lat_p50s.push_back(r.lat_p50);
      lat_p99s.push_back(r.lat_p99);
    }

    entry.throughput_stats = compute_stats(throughputs);
    entry.lat_p50_stats = compute_stats(lat_p50s);
    entry.lat_p99_stats = compute_stats(lat_p99s);

    summary.push_back(std::move(entry));
  }

  csv.write_summary(summary);

  spdlog::info("Done. Results written to:");
  spdlog::info("  Raw:     {}", csv.raw_path());
  spdlog::info("  Summary: {}", csv.summary_path());

  // ---- Cleanup ----
  // cds_global_terminate();
  return 0;
}
