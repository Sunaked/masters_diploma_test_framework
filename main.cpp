// ============================================================================
// main.cpp — точка входа бенчмарк-фреймворка.
//
// ============================================================================

#include "containers/ContainerAdapter.hpp"
// #include "containers/CDS_Adapters.hpp"
// #include "containers/CK_HT_Adapter.hpp"
#include "containers/CustomAdapter.hpp"
#include "executor/MultiThreadExecutor.hpp"
#include "metrics/Statistics.hpp"
#include "output/CsvWriter.hpp"
#include "workload/Phase.hpp"
#include "workload/ScriptedGenerator.hpp"

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
// Конфигурационные структуры
// ============================================================================

struct GlobalConfig {
  double target_total_time_minutes = 240;
  uint32_t base_runs_per_config = 5;
  uint32_t min_runs_per_config = 3;
  uint32_t metrics_sampling_ms = 200;
  bool pin_threads = true;
  bool use_rdtsc_for_latency = true;

  bool latency_off = false;
  bool sampler_off = false;
  std::string latency_mode = "cycles"; // "cycles", "wall_ns", "both"
  size_t reservoir_capacity = 1'000'000;
  double warmup_auto_seconds = 0.0;
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

  g.latency_off = tbl.get_or("latency_off", false);
  g.sampler_off = tbl.get_or("sampler_off", false);
  g.latency_mode = tbl.get_or<std::string>("latency_mode", "cycles");
  g.reservoir_capacity =
      tbl.get_or("reservoir_capacity", static_cast<uint64_t>(1'000'000));
  g.warmup_auto_seconds = tbl.get_or("warmup_auto_seconds", 0.0);

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

PlanKeyMode parse_plan_key_mode(const std::string &s) {
  if (s == "sequential")
    return PlanKeyMode::Sequential;
  if (s == "fixed")
    return PlanKeyMode::Fixed;
  if (s == "uniform")
    return PlanKeyMode::Uniform;
  if (s == "zipfian")
    return PlanKeyMode::Zipfian;
  throw std::runtime_error("Unknown plan key_mode: '" + s + "'");
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

      std::string mode_str = p.get_or<std::string>("mode", "probabilistic");

      // phase role
      std::string role_str = p.get_or<std::string>("role", "measure");
      ph.role = parse_phase_role(role_str);

      if (mode_str == "probabilistic") {
        ph.mode = PhaseMode::Probabilistic;
        ph.ops_per_thread = p.get_or("ops_per_thread", 10000ULL);
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

      } else if (mode_str == "scripted_step") {
        ph.mode = PhaseMode::ScriptedStep;
        ph.ops_per_thread = p.get_or("ops_per_thread", 10000ULL);
        ph.lua_generator_name = p["script"].get<std::string>();
        ph.key_range = p.get_or("key_range", 1000000ULL);
        ph.alpha = p.get_or("alpha", 0.99);

        spdlog::info("Loaded ScriptedStep: scenario='{}', gen='{}', ops={}",
                     sc.name, ph.lua_generator_name, ph.ops_per_thread);

      } else if (mode_str == "scripted_plan") {
        ph.mode = PhaseMode::ScriptedPlan;

        sol::table plan = p["plan"];
        for (size_t k = 1; k <= plan.size(); ++k) {
          sol::table entry = plan[k];
          PlanEntry pe;

          pe.count = entry["count"].get<uint64_t>();

          std::string op_str = entry["op"].get<std::string>();
          if (!parse_op_type(op_str, pe.op)) {
            throw std::runtime_error(
                "Invalid op '" + op_str + "' in ScriptedPlan entry " +
                std::to_string(k) + " of scenario '" + sc.name + "'");
          }

          std::string km = entry.get_or<std::string>("key_mode", "sequential");
          pe.key_mode = parse_plan_key_mode(km);
          pe.start = entry.get_or("start", 0ULL);
          pe.fixed_key = entry.get_or("fixed_key", 0ULL);
          pe.key_range = entry.get_or("key_range", 1000000ULL);
          pe.alpha = entry.get_or("alpha", 0.99);

          ph.plan_entries.push_back(pe);
        }

        uint64_t total = 0;
        for (const auto &e : ph.plan_entries)
          total += e.count;
        ph.ops_per_thread = total;

        spdlog::info("Loaded ScriptedPlan: scenario='{}', entries={}, ops={}",
                     sc.name, ph.plan_entries.size(), ph.ops_per_thread);

      } else {
        throw std::runtime_error("Unknown phase mode '" + mode_str +
                                 "' in scenario '" + sc.name + "'");
      }

      sc.phases.push_back(std::move(ph));
    }
    result.push_back(std::move(sc));
  }
  return result;
}

// ============================================================================
// Фабрика контейнеров
// ============================================================================

// NullContainer support
std::unique_ptr<ContainerBase> create_container(const std::string &type) {
  if (type == "custom") {
    return std::make_unique<ContainerBridge<ShardedHashMap<256>>>();
  }
  if (type == "null") {
    return std::make_unique<NullContainer>();
  }
  spdlog::error("Unknown container type: '{}'", type);
  return nullptr;
}

// ============================================================================
// Логика распределения времени
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

    uint32_t num_configs =
        static_cast<uint32_t>(scenarios.size() * threads_list.size());

    uint32_t runs_per_config =
        (num_configs > 0) ? std::max(global.min_runs_per_config, num_configs)
                          : global.base_runs_per_config;

    spdlog::debug("compute_run_plans, container_name='{}', "
                  "container_type='{}', runs_per_config='{}', "
                  "min_runs_per_config='{}', base_runs_per_config='{}'",
                  plan.container_name, plan.container_type, runs_per_config,
                  global.min_runs_per_config, global.base_runs_per_config);
    for (const auto &sc : scenarios) {
      for (uint32_t thr : threads_list) {
        plan.configs.push_back({sc.name, thr, runs_per_config});
      }
    }

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
  spdlog::set_level(spdlog::level::debug);
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

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

  spdlog::info("Loaded: {} containers, {} scenarios, {} thread counts",
               container_cfg.size(), scenarios.size(), threads_list.size());

  // ---- Валидация scripted-фаз ----
  try {
    validate_scripted_phases(scenarios, lua);
  } catch (const std::runtime_error &e) {
    spdlog::error("Scripted phase validation failed: {}", e.what());
    return 1;
  }

  // ---- Расчёт бюджета времени ----
  auto plans =
      compute_run_plans(container_cfg, scenarios, threads_list, global_cfg);

  // ---- Подготовка вывода ----
  CsvWriter csv("results");
  csv.write_raw_header();

  // ---- Дамп входных параметров ----
  csv.dump_config_global(
      global_cfg.target_total_time_minutes, global_cfg.base_runs_per_config,
      global_cfg.min_runs_per_config, global_cfg.metrics_sampling_ms,
      global_cfg.pin_threads, global_cfg.use_rdtsc_for_latency, threads_list);

  {
    std::vector<std::string> names, types;
    std::vector<double> minutes;
    for (const auto &cc : container_cfg) {
      names.push_back(cc.name);
      types.push_back(cc.type);
      minutes.push_back(cc.target_time_minutes);
    }
    csv.dump_config_containers(names, types, minutes);
  }

  csv.dump_config_scenarios(scenarios);

  // ---- Основной цикл бенчмарка ----
  std::map<std::string, std::vector<RunResult>> results_by_key;

  ExecutorConfig exec_cfg;
  exec_cfg.pin_threads = global_cfg.pin_threads;
  exec_cfg.use_rdtsc = global_cfg.use_rdtsc_for_latency;
  exec_cfg.metrics_sampling_ms = global_cfg.metrics_sampling_ms;
  exec_cfg.dump_ops = true;

  exec_cfg.latency_off = global_cfg.latency_off;
  exec_cfg.sampler_off = global_cfg.sampler_off;
  exec_cfg.reservoir_capacity = global_cfg.reservoir_capacity;
  exec_cfg.warmup_auto_seconds = global_cfg.warmup_auto_seconds;

  if (global_cfg.latency_mode == "wall_ns")
    exec_cfg.latency_mode = LatencyMode::WallNs;
  else if (global_cfg.latency_mode == "both")
    exec_cfg.latency_mode = LatencyMode::Both;
  else
    exec_cfg.latency_mode = LatencyMode::Cycles;

  for (const auto &plan : plans) {
    spdlog::info("=== Container: {} (type: {}) ===", plan.container_name,
                 plan.container_type);

    for (const auto &cfg : plan.configs) {
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
        auto container = create_container(plan.container_type);
        if (!container) {
          spdlog::error("Skipping container '{}'", plan.container_name);
          continue;
        }
        spdlog::debug("Created container ({})", plan.container_type);

        auto rr = execute_run(plan.container_name, *container, *scenario,
                              cfg.thread_count, run, exec_cfg, csv, lua);

        std::string key = plan.container_name + "|" + cfg.scenario_name + "|" +
                          std::to_string(cfg.thread_count);
        results_by_key[key].push_back(std::move(rr));
        spdlog::debug("Deleted ({})", key);
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

  spdlog::info("Done. Output directory: {}", csv.run_dir());
  spdlog::info("  Raw:     {}", csv.raw_path());
  spdlog::info("  Summary: {}", csv.summary_path());
  spdlog::info("  Ops:     {}", csv.ops_dir());
  spdlog::info("  Phases:  {}", csv.phase_results_path());

  return 0;
}
