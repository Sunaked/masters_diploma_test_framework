#pragma once
// ============================================================================
// CustomAdapter.hpp — пример пользовательского контейнера.
//
// Демонстрирует, как подключить собственную реализацию:
//   1. Наследоваться от ContainerCRTP<YourClass>
//   2. Реализовать do_insert, do_find, do_erase, do_clear, do_name
//   3. Зарегистрировать фабрику в main.cpp
//
// Здесь приведён простой concurrent hash map на основе sharded
// std::unordered_map с per-shard std::shared_mutex (readers-writer lock). Это
// baseline для сравнения с lock-free реализациями.
// ============================================================================

#include "ContainerAdapter.hpp"

#include <array>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace bench {

/// Sharded hash map с fine-grained locking.
/// Количество шардов = степень двойки для быстрого маскирования.
/// Это типичный baseline-контейнер для сравнения с lock-free структурами.
template <size_t NumShards = 256>
class ShardedHashMap : public ContainerCRTP<ShardedHashMap<NumShards>> {
  static_assert((NumShards & (NumShards - 1)) == 0,
                "NumShards must be power of 2");

public:
  bool do_insert(uint64_t key, uint64_t value) {
    auto &shard = get_shard(key);
    std::unique_lock lock(shard.mtx);
    auto [it, inserted] = shard.map.emplace(key, value);
    if (!inserted)
      it->second = value; // upsert semantics
    return true;
  }

  bool do_find(uint64_t key, uint64_t &value) {
    auto &shard = get_shard(key);
    std::shared_lock lock(shard.mtx);
    auto it = shard.map.find(key);
    if (it != shard.map.end()) {
      value = it->second;
      return true;
    }
    return false;
  }

  bool do_erase(uint64_t key) {
    auto &shard = get_shard(key);
    std::unique_lock lock(shard.mtx);
    return shard.map.erase(key) > 0;
  }

  void do_clear() {
    for (auto &shard : shards_) {
      std::unique_lock lock(shard.mtx);
      shard.map.clear();
    }
  }

  std::string do_name() const { return "ShardedHashMap"; }

private:
  struct Shard {
    alignas(64) std::shared_mutex mtx; // каждый shard на отдельной cache line
    std::unordered_map<uint64_t, uint64_t> map;
  };

  std::array<Shard, NumShards> shards_;

  Shard &get_shard(uint64_t key) {
    // Fibonacci hashing для лучшего распределения по шардам
    constexpr uint64_t kFibMul = 11400714819323198485ULL; // 2^64 / φ
    size_t idx = static_cast<size_t>((key * kFibMul) >>
                                     (64 - __builtin_ctzll(NumShards)));
    return shards_[idx];
  }
};

} // namespace bench
