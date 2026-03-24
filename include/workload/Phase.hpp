#pragma once
// ============================================================================
// Phase.hpp — описание одной фазы сценария нагрузки.
//
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

enum class PhaseRole : uint8_t {
  Measure = 0,  // DEFAULT: timed, stats recorded in raw_results.csv
  Prefill = 1,  // Runs OUTSIDE timing entirely. No latency, no stats.
  Warmup = 2,   // Runs inside timing window but stats DISCARDED from final.
  Cooldown = 3, // Runs after measurement. Stats discarded.
};

inline const char *to_string(PhaseRole r) {
  switch (r) {
  case PhaseRole::Measure:
    return "measure";
  case PhaseRole::Prefill:
    return "prefill";
  case PhaseRole::Warmup:
    return "warmup";
  case PhaseRole::Cooldown:
    return "cooldown";
  }
  return "unknown";
}

inline PhaseRole parse_phase_role(const std::string &s) {
  if (s == "prefill")
    return PhaseRole::Prefill;
  if (s == "warmup")
    return PhaseRole::Warmup;
  if (s == "cooldown")
    return PhaseRole::Cooldown;
  return PhaseRole::Measure; // default for backward compat
}

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
  Sequential = 0,
  Fixed = 1,
  Uniform = 2,
  Zipfian = 3,
};

struct PlanEntry {
  uint64_t count = 0;
  OpType op = OpType::Insert;
  PlanKeyMode key_mode = PlanKeyMode::Sequential;
  uint64_t start = 0;
  uint64_t fixed_key = 0;
  uint64_t key_range = 1'000'000;
  double alpha = 0.99;
};

// ---------------------------------------------------------------------------
// Phase
// ---------------------------------------------------------------------------

struct Phase {
  PhaseMode mode = PhaseMode::Probabilistic;

  PhaseRole role = PhaseRole::Measure; // default = measured (backward compat)

  uint64_t ops_per_thread = 500;

  // ---------- Probabilistic ----------
  double insert_ratio = 0.0;
  double find_ratio = 1.0;
  double erase_ratio = 0.0;
  KeyDist key_dist = KeyDist::Uniform;
  double alpha = 0.99;
  uint64_t key_range = 1'000'000;

  // ---------- ScriptedStep ----------
  std::string lua_generator_name;

  // ---------- ScriptedPlan ----------
  std::vector<PlanEntry> plan_entries;
};

struct Scenario {
  std::string name;
  std::vector<Phase> phases;
};

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
