#pragma once
// ============================================================================
// ScriptedGenerator.hpp — Lua-based trace generation.
//
// КРИТИЧЕСКИЙ ИНВАРИАНТ:
//   Lua вызывается ТОЛЬКО на этапе прегенерации trace → vector<Op>.
//   Горячий цикл бенчмарка НИКОГДА не обращается к Lua.
//
// Два режима:
//
//   ScriptedStep — Lua callback вызывается для каждого (tid, i):
//     function gen(ctx, i)
//       return { op = "insert", key = i * 2 }
//     end
//
//   ScriptedPlan — декларативный план, C++ генерирует trace без Lua:
//     plan = {
//       { count = 10000, op = "insert", key_mode = "sequential", start = 0 },
//       { count = 50000, op = "find",   key_mode = "fixed", fixed_key = 42 },
//     }
//
// Валидация:
//   - Функция существует в Lua state
//   - op корректный ("insert" / "find" / "erase")
//   - key — число
//   - ops_per_thread > 0
//   - Ошибки Lua не проглатываются молча
//
// Debug-логи: первые 5 операций каждой фазы логируются при генерации.
// ============================================================================

#include "Phase.hpp"
#include "ZipfianGenerator.hpp"

#include <sol/sol.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {

// ============================================================================
// Валидация scripted-фаз (вызвать ДО запуска бенчмарка)
// ============================================================================

/// Проверяет, что все scripted-фазы корректны. Бросает std::runtime_error.
inline void validate_scripted_phases(const std::vector<Scenario> &scenarios,
                                     sol::state &lua) {
  for (const auto &sc : scenarios) {
    for (size_t pi = 0; pi < sc.phases.size(); ++pi) {
      const auto &phase = sc.phases[pi];

      if (phase.mode == PhaseMode::ScriptedStep) {
        // Проверяем: функция существует
        if (phase.lua_generator_name.empty()) {
          throw std::runtime_error("ScriptedStep phase " + std::to_string(pi) +
                                   " in scenario '" + sc.name +
                                   "' has empty lua_generator_name");
        }

        sol::object fn = lua[phase.lua_generator_name];
        if (!fn.is<sol::function>()) {
          throw std::runtime_error("Lua function '" + phase.lua_generator_name +
                                   "' not found for ScriptedStep phase " +
                                   std::to_string(pi) + " in scenario '" +
                                   sc.name + "'");
        }

        if (phase.ops_per_thread == 0) {
          throw std::runtime_error("ScriptedStep phase " + std::to_string(pi) +
                                   " in scenario '" + sc.name +
                                   "' has ops_per_thread == 0");
        }

        spdlog::info("Validated ScriptedStep: scenario='{}', phase={}, "
                     "generator='{}', ops_per_thread={}",
                     sc.name, pi, phase.lua_generator_name,
                     phase.ops_per_thread);
      }

      if (phase.mode == PhaseMode::ScriptedPlan) {
        if (phase.plan_entries.empty()) {
          throw std::runtime_error("ScriptedPlan phase " + std::to_string(pi) +
                                   " in scenario '" + sc.name +
                                   "' has empty plan_entries");
        }

        uint64_t total_ops = 0;
        for (size_t ei = 0; ei < phase.plan_entries.size(); ++ei) {
          const auto &e = phase.plan_entries[ei];
          if (e.count == 0) {
            throw std::runtime_error("ScriptedPlan entry " +
                                     std::to_string(ei) + " in phase " +
                                     std::to_string(pi) + " of scenario '" +
                                     sc.name + "' has count == 0");
          }
          total_ops += e.count;
        }

        spdlog::info("Validated ScriptedPlan: scenario='{}', phase={}, "
                     "entries={}, total_ops_per_thread={}",
                     sc.name, pi, phase.plan_entries.size(), total_ops);
      }
    }
  }
}

// ============================================================================
// ScriptedStep: генерация через Lua callback
// ============================================================================

/// Генерирует trace для одного потока через Lua callback.
///
/// Lua-функция вызывается ops_per_thread раз:
///   result = gen(ctx_table, i)
///   result.op  → "insert" / "find" / "erase"
///   result.key → uint64_t
///
/// Все вызовы Lua происходят здесь, ДО горячего цикла.
inline std::vector<Op>
generate_scripted_step_ops(sol::state &lua, const Phase &phase,
                           const std::string &scenario_name, size_t phase_index,
                           uint64_t thread_id, uint64_t thread_count,
                           uint64_t seed) {
  const uint64_t n = phase.ops_per_thread;
  std::vector<Op> ops;
  ops.reserve(n);

  spdlog::debug("Lua scripted phase: scenario='{}', phase={}, generator='{}', "
                "ops_per_thread={}, tid={}",
                scenario_name, phase_index, phase.lua_generator_name, n,
                thread_id);

  // Получаем Lua-функцию
  sol::function gen = lua[phase.lua_generator_name];

  // Строим ctx table для Lua
  sol::table ctx = lua.create_table();
  ctx["scenario_name"] = scenario_name;
  ctx["phase_index"] = phase_index;
  ctx["thread_id"] = thread_id;
  ctx["thread_count"] = thread_count;
  ctx["ops_per_thread"] = n;
  ctx["seed"] = seed;
  ctx["key_range"] = phase.key_range;
  ctx["alpha"] = phase.alpha;

  for (uint64_t i = 1; i <= n; ++i) {
    // Lua вызов: result = gen(ctx, i)
    sol::protected_function_result result = gen(ctx, i);

    if (!result.valid()) {
      sol::error err = result;
      throw std::runtime_error("Lua generator '" + phase.lua_generator_name +
                               "' error at i=" + std::to_string(i) + ", tid=" +
                               std::to_string(thread_id) + ": " + err.what());
    }

    Op op{};

    // Два формата: table {op=..., key=...} или multi-return op, key
    sol::object val = result;
    if (val.is<sol::table>()) {
      sol::table t = val;
      std::string op_str = t["op"].get<std::string>();
      if (!parse_op_type(op_str, op.type)) {
        throw std::runtime_error("Lua generator '" + phase.lua_generator_name +
                                 "' returned invalid op='" + op_str +
                                 "' at i=" + std::to_string(i));
      }
      op.key = t["key"].get<uint64_t>();
    } else {
      // Multi-return: op_str, key
      std::string op_str = result.get<std::string>(0);
      uint64_t key_val = result.get<uint64_t>(1);
      if (!parse_op_type(op_str, op.type)) {
        throw std::runtime_error("Lua generator '" + phase.lua_generator_name +
                                 "' returned invalid op='" + op_str +
                                 "' at i=" + std::to_string(i));
      }
      op.key = key_val;
    }

    // Debug preview: первые 5 операций
    if (i <= 5) {
      spdlog::debug("Lua op preview: scenario='{}', phase={}, tid={}, "
                    "i={}, op={}, key={}",
                    scenario_name, phase_index, thread_id, i,
                    to_string(op.type), op.key);
    }

    ops.push_back(op);
  }

  spdlog::debug(
      "Lua phase generated: scenario='{}', phase={}, tid={}, total_ops={}",
      scenario_name, phase_index, thread_id, ops.size());

  return ops;
}

// ============================================================================
// ScriptedPlan: генерация из декларативного плана (без Lua-вызовов)
// ============================================================================

/// Генерирует trace для одного потока из декларативного плана.
/// Вычисляется полностью в C++, Lua не требуется.
inline std::vector<Op> generate_scripted_plan_ops(const Phase &phase,
                                                  uint64_t thread_id,
                                                  uint64_t seed) {
  // Подсчитываем общее количество операций
  uint64_t total = 0;
  for (const auto &e : phase.plan_entries) {
    total += e.count;
  }

  std::vector<Op> ops;
  ops.reserve(total);

  std::mt19937_64 rng(seed ^ (thread_id * 2654435761ULL));

  for (size_t ei = 0; ei < phase.plan_entries.size(); ++ei) {
    const auto &entry = phase.plan_entries[ei];

    // Генераторы ключей по key_mode
    std::unique_ptr<ZipfianGenerator> zipf;
    std::uniform_int_distribution<uint64_t> uniform_dist;

    if (entry.key_mode == PlanKeyMode::Uniform) {
      uniform_dist = std::uniform_int_distribution<uint64_t>(
          0, entry.key_range > 0 ? entry.key_range - 1 : 0);
    } else if (entry.key_mode == PlanKeyMode::Zipfian) {
      zipf = std::make_unique<ZipfianGenerator>(entry.key_range, entry.alpha,
                                                seed + ei * 999983 + thread_id);
    }

    uint64_t seq_counter = entry.start + thread_id;

    for (uint64_t i = 0; i < entry.count; ++i) {
      Op op{};
      op.type = entry.op;

      switch (entry.key_mode) {
      case PlanKeyMode::Sequential:
        op.key = seq_counter++;
        break;
      case PlanKeyMode::Fixed:
        op.key = entry.fixed_key;
        break;
      case PlanKeyMode::Uniform:
        op.key = uniform_dist(rng);
        break;
      case PlanKeyMode::Zipfian:
        op.key = zipf->next();
        break;
      }

      ops.push_back(op);
    }
  }

  return ops;
}

} // namespace bench
