#pragma once
// ============================================================================
// Phase.hpp — описание одной фазы сценария нагрузки.
//
// Три режима генерации trace (PhaseMode):
//
//   1. Probabilistic — классический YCSB-стиль: вероятностный вектор
//      w = (p_insert, p_find, p_erase) + распределение ключей.
//      Формализация:  S = (w, D_key, N_m)
//
//   2. ScriptedStep — Lua-функция задаёт каждую операцию по индексу:
//        function gen(ctx, i) return { op = "insert", key = i } end
//      Вызывается ТОЛЬКО при прегенерации. Горячий цикл не трогает Lua.
//
//   3. ScriptedPlan — декларативный план высокого уровня:
//        { count = 10000, op = "insert", key_mode = "sequential" }
//      Компактно описывает bursts, prefill, hotspot-ы.
//
// Методологическое требование: для одного (scenario, threads, run_index)
// все контейнеры получают ИДЕНТИЧНЫЙ trace (один seed → один trace).
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// OpType
// ---------------------------------------------------------------------------

enum class OpType : uint8_t {
  Insert = 0,
  Find = 1,
  Erase = 2,
};

inline const char *to_string(OpType t) {
  switch (t) {
  case OpType::Insert:
    return "insert";
  case OpType::Find:
    return "find";
  case OpType::Erase:
    return "erase";
  }
  return "unknown";
}

/// Парсинг строки → OpType. Возвращает false при неизвестном значении.
inline bool parse_op_type(const std::string &s, OpType &out) {
  if (s == "insert") {
    out = OpType::Insert;
    return true;
  }
  if (s == "find") {
    out = OpType::Find;
    return true;
  }
  if (s == "erase") {
    out = OpType::Erase;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// KeyDist, PhaseMode
// ---------------------------------------------------------------------------

enum class KeyDist : uint8_t {
  Uniform = 0,
  Zipfian = 1,
  Sequential = 2,
};

enum class PhaseMode : uint8_t {
  Probabilistic = 0,
  ScriptedStep = 1,
  ScriptedPlan = 2,
};

// ---------------------------------------------------------------------------
// Op — одна предвычисленная операция
// ---------------------------------------------------------------------------

struct alignas(16) Op {
  uint64_t key;
  OpType type;
};
static_assert(sizeof(Op) == 16,
              "Op should be 16 bytes for cache-line alignment");

// ---------------------------------------------------------------------------
// ScriptedPlan: шаг декларативного плана
// ---------------------------------------------------------------------------

enum class PlanKeyMode : uint8_t {
  Sequential = 0, ///< key = start + i
  Fixed = 1,      ///< key = fixed_key (hotspot)
  Uniform = 2,    ///< key = random uniform [0, key_range)
  Zipfian = 3,    ///< key = Zipfian(key_range, alpha)
};

/// Один шаг декларативного плана.
///
/// Lua-пример:
///   { count = 10000, op = "insert", key_mode = "sequential", start = 0 }
///   { count = 50000, op = "find",   key_mode = "fixed",      fixed_key = 42 }
struct PlanEntry {
  uint64_t count = 0;
  OpType op = OpType::Insert;
  PlanKeyMode key_mode = PlanKeyMode::Sequential;
  uint64_t start = 0;             ///< Sequential: начальный ключ
  uint64_t fixed_key = 0;         ///< Fixed: значение ключа
  uint64_t key_range = 1'000'000; ///< Uniform / Zipfian
  double alpha = 0.99;            ///< Zipfian
};

// ---------------------------------------------------------------------------
// Phase
// ---------------------------------------------------------------------------

struct Phase {
  PhaseMode mode = PhaseMode::Probabilistic;

  /// Количество операций на поток.
  /// - Probabilistic: обязательно
  /// - ScriptedStep: обязательно (сколько раз вызвать Lua-функцию)
  /// - ScriptedPlan: игнорируется (определяется суммой count)
  uint64_t ops_per_thread = 500;

  // ---------- Probabilistic ----------
  double insert_ratio = 0.0;
  double find_ratio = 1.0;
  double erase_ratio = 0.0;
  KeyDist key_dist = KeyDist::Uniform;
  double alpha = 0.99;
  uint64_t key_range = 1'000'000;

  // ---------- ScriptedStep ----------
  /// Имя Lua-функции: function name(ctx, i) → {op="...", key=N}
  std::string lua_generator_name;

  // ---------- ScriptedPlan ----------
  std::vector<PlanEntry> plan_entries;
};

/// Полный сценарий = имя + упорядоченный набор фаз.
struct Scenario {
  std::string name;
  std::vector<Phase> phases;
};

// ---------------------------------------------------------------------------
// Контекст для Lua scripted генератора
// ---------------------------------------------------------------------------

/// Передаётся в Lua-функцию как ctx. Не содержит живого состояния контейнера.
struct LuaPhaseContext {
  std::string scenario_name;
  uint64_t phase_index = 0;
  uint64_t thread_id = 0;
  uint64_t thread_count = 0;
  uint64_t ops_per_thread = 0;
  uint64_t seed = 0;
  uint64_t key_range = 1'000'000;
  double alpha = 0.99;
};

} // namespace bench
