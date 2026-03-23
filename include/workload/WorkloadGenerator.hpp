#pragma once
// ============================================================================
// WorkloadGenerator.hpp — предвычисление операций для бенчмарка.
//
// Маршрутизация по PhaseMode:
//   Probabilistic → generate_phase_ops() — классический YCSB
//   ScriptedStep  → generate_scripted_step_ops() — Lua callback
//   ScriptedPlan  → generate_scripted_plan_ops() — декларативный план
//
// Ключевой инвариант: ВСЕ генерация (включая Lua-вызовы для ScriptedStep)
// происходит ДО старта замера. Горячий цикл итерирует по vector<Op>.
//
// Методологическое требование: trace генерируется с фиксированным seed,
// идентичным для всех контейнеров при одном (scenario, threads, run_index).
// ============================================================================

#include "Phase.hpp"
#include "ScriptedGenerator.hpp"
#include "ZipfianGenerator.hpp"

#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

namespace bench {

/// Параметры предвычисления.
struct WorkloadConfig {
  uint64_t base_seed = 12345;
};

// ============================================================================
// Probabilistic mode: классический YCSB
// ============================================================================

inline std::vector<Op>
generate_phase_ops(const Phase &phase, uint64_t thread_id, uint64_t base_seed) {
  const uint64_t total_ops = phase.ops_per_thread;
  std::vector<Op> ops;
  ops.reserve(total_ops);

  const uint64_t seed = base_seed ^ (thread_id * 2654435761ULL);

  std::mt19937_64 rng(seed);
  std::unique_ptr<ZipfianGenerator> zipf;

  if (phase.key_dist == KeyDist::Zipfian) {
    zipf = std::make_unique<ZipfianGenerator>(phase.key_range, phase.alpha,
                                              seed + 1);
  }

  std::uniform_int_distribution<uint64_t> uniform_key(0, phase.key_range - 1);
  uint64_t sequential_counter = thread_id;

  const double threshold_insert = phase.insert_ratio;
  const double threshold_find = phase.insert_ratio + phase.find_ratio;
  std::uniform_real_distribution<double> op_dist(0.0, 1.0);

  for (uint64_t i = 0; i < total_ops; ++i) {
    Op op{};

    double r = op_dist(rng);
    if (r < threshold_insert)
      op.type = OpType::Insert;
    else if (r < threshold_find)
      op.type = OpType::Find;
    else
      op.type = OpType::Erase;

    switch (phase.key_dist) {
    case KeyDist::Uniform:
      op.key = uniform_key(rng);
      break;
    case KeyDist::Zipfian:
      op.key = zipf->next();
      break;
    case KeyDist::Sequential:
      op.key = sequential_counter++;
      break;
    }

    ops.push_back(op);
  }

  return ops;
}

// ============================================================================
// Главная точка входа: генерация для всех потоков и фаз
// ============================================================================

/// Генерирует trace для ВСЕХ потоков и ВСЕХ фаз сценария.
///
/// Возвращает: ops[phase_index][thread_id] = vector<Op>
///
/// @param lua  sol::state — нужен ТОЛЬКО для ScriptedStep фаз.
///             Вызовы Lua происходят здесь, до горячего цикла.
///             Для Probabilistic и ScriptedPlan Lua не используется.
inline std::vector<std::vector<std::vector<Op>>>
generate_scenario_ops(const Scenario &scenario, uint32_t thread_count,
                      const WorkloadConfig &config, sol::state &lua) {
  std::vector<std::vector<std::vector<Op>>> all_ops;
  all_ops.reserve(scenario.phases.size());

  for (size_t pi = 0; pi < scenario.phases.size(); ++pi) {
    const auto &phase = scenario.phases[pi];
    std::vector<std::vector<Op>> phase_ops(thread_count);

    spdlog::debug("Generating phase {}/{}: mode={}, scenario='{}'", pi + 1,
                  scenario.phases.size(), static_cast<int>(phase.mode),
                  scenario.name);

    for (uint32_t tid = 0; tid < thread_count; ++tid) {
      uint64_t phase_seed = config.base_seed + pi * 1'000'000 + tid;

      switch (phase.mode) {

      case PhaseMode::Probabilistic:
        phase_ops[tid] = generate_phase_ops(phase, tid, phase_seed);
        break;

      case PhaseMode::ScriptedStep:
        phase_ops[tid] = generate_scripted_step_ops(
            lua, phase, scenario.name, pi, tid, thread_count, phase_seed);
        break;

      case PhaseMode::ScriptedPlan:
        phase_ops[tid] = generate_scripted_plan_ops(phase, tid, phase_seed);
        break;
      }

      spdlog::debug("Phase {}, tid {}: generated {} ops (mode={})", pi, tid,
                    phase_ops[tid].size(), static_cast<int>(phase.mode));
    }

    all_ops.push_back(std::move(phase_ops));
  }

  return all_ops;
}

} // namespace bench
