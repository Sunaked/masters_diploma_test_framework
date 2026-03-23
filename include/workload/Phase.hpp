#pragma once
// ============================================================================
// Phase.hpp — описание одной фазы сценария нагрузки.
//
// Фаза определяет:
//   - длительность (мс)
//   - пропорции операций (insert / find / erase), сумма ≤ 1.0
//   - распределение ключей (uniform, zipfian, sequential)
//   - параметры распределения (alpha для Zipfian, key_range)
//
// Все фазы считываются из Lua до старта замеров. Во время горячего цикла
// используется только предвычисленный std::vector<Op>.
// ============================================================================

#include <cstdint>
#include <string>

namespace bench {

/// Тип операции над контейнером.
enum class OpType : uint8_t {
  Insert = 0,
  Find = 1,
  Erase = 2,
};

/// Тип распределения ключей.
enum class KeyDist : uint8_t {
  Uniform = 0,
  Zipfian = 1,
  Sequential = 2,
};

/// Одна предвычисленная операция (8 байт ключ + 1 байт тип = 9 байт,
/// выравнивание до 16). Компактное представление минимизирует cache pollution
/// при итерации.
struct alignas(16) Op {
  uint64_t key;
  OpType type;
};
static_assert(sizeof(Op) == 16,
              "Op should be 16 bytes for cache-line alignment");

/// Описание одной фазы (считывается из Lua, неизменяемо после загрузки).
struct Phase {
  uint64_t ops_per_thread = 500;
  double insert_ratio = 0.0;
  double find_ratio = 1.0;
  double erase_ratio = 0.0;
  KeyDist key_dist = KeyDist::Uniform;
  double alpha = 0.99; ///< параметр Zipf (только для key_dist == Zipfian)
  uint64_t key_range = 1'000'000;
};

/// Полный сценарий = имя + упорядоченный набор фаз.
struct Scenario {
  std::string name;
  std::vector<Phase> phases;
};

} // namespace bench
