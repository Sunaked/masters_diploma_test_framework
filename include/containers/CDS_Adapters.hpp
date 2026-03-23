#pragma once
// ============================================================================
// CDS_Adapters.hpp — адаптеры для libcds lock-free контейнеров.
//
// libcds (Concurrent Data Structures library, Хижняк М.А.) предоставляет
// набор lock-free и lock-based контейнеров, реализованных на C++11/14.
//
// Контейнеры:
//   - SplitListMap: lock-free hash map на основе split-ordered list
//     (Shalev & Shavit, "Split-Ordered Lists: Lock-Free Extensible Hash
//     Tables",
//      JACM 53(3), 2006)
//   - FeldmanHashMap: lock-free hash map на основе Feldman hash trie
//     (Feldman et al., "A Wait-Free Hash Map", JPDC 2013)
//
// ВАЖНО: libcds требует инициализации потока через cds::threading::Manager.
// Каждый рабочий поток ОБЯЗАН вызвать cds::threading::Manager::attachThread()
// до начала работы и detachThread() при завершении.
//
// Ссылки:
//   [2] https://github.com/khizmax/libcds
// ============================================================================

#include "ContainerAdapter.hpp"

#ifndef NO_LIBCDS

#include <cds/container/feldman_hashmap_hp.h>
#include <cds/container/split_list_map.h>
#include <cds/gc/hp.h> // Hazard Pointer GC
#include <cds/init.h>

namespace bench {

// ---------------------------------------------------------------------------
// SplitListMap адаптер
// ---------------------------------------------------------------------------

class SplitListMapAdapter : public ContainerCRTP<SplitListMapAdapter> {
public:
  // Типы для SplitListMap: ключ uint64_t → значение uint64_t
  struct Traits
      : public cds::container::split_list::make_traits<
            cds::container::split_list::ordered_list_traits<
                cds::container::michael_list::make_traits<
                    cds::opt::less<std::less<uint64_t>>>::type>>::type {};

  using map_type =
      cds::container::SplitListMap<cds::gc::HP, uint64_t, uint64_t, Traits>;

  bool do_insert(uint64_t key, uint64_t value) {
    return map_.insert(key, value);
  }

  bool do_find(uint64_t key, uint64_t &value) {
    return map_.find(key, [&value](auto const &pair) { value = pair.second; });
  }

  bool do_erase(uint64_t key) { return map_.erase(key); }

  void do_clear() { map_.clear(); }

  std::string do_name() const { return "SplitListMap"; }

private:
  map_type map_;
};

// ---------------------------------------------------------------------------
// FeldmanHashMap адаптер
// ---------------------------------------------------------------------------

class FeldmanHashMapAdapter : public ContainerCRTP<FeldmanHashMapAdapter> {
public:
  struct Traits : public cds::container::feldman_hashmap::traits {
    // FeldmanHashMap требует, чтобы ключ был тривиально хешируемым.
    // uint64_t — отлично подходит (identity hash).
  };

  using map_type =
      cds::container::FeldmanHashMap<cds::gc::HP, uint64_t, uint64_t, Traits>;

  bool do_insert(uint64_t key, uint64_t value) {
    return map_.insert(key, value);
  }

  bool do_find(uint64_t key, uint64_t &value) {
    return map_.find(key, [&value](auto const &pair) { value = pair.second; });
  }

  bool do_erase(uint64_t key) { return map_.erase(key); }

  void do_clear() { map_.clear(); }

  std::string do_name() const { return "FeldmanHashMap"; }

private:
  map_type map_;
};

/// RAII-guard для инициализации libcds в потоке.
struct CdsThreadGuard {
  CdsThreadGuard() { cds::threading::Manager::attachThread(); }
  ~CdsThreadGuard() { cds::threading::Manager::detachThread(); }
};

/// Глобальная инициализация libcds (вызвать один раз в main).
inline void cds_global_init() {
  cds::Initialize();
  // Инициализация Hazard Pointer GC с достаточным числом guard'ов
  cds::gc::HP hpGC(128); // 128 hazard pointers
}

inline void cds_global_terminate() { cds::Terminate(); }

} // namespace bench

#else // NO_LIBCDS — заглушки

namespace bench {

class SplitListMapAdapter : public ContainerCRTP<SplitListMapAdapter> {
public:
  bool do_insert(uint64_t, uint64_t) { return true; }
  bool do_find(uint64_t, uint64_t &) { return false; }
  bool do_erase(uint64_t) { return true; }
  void do_clear() {}
  std::string do_name() const { return "SplitListMap(stub)"; }
};

class FeldmanHashMapAdapter : public ContainerCRTP<FeldmanHashMapAdapter> {
public:
  bool do_insert(uint64_t, uint64_t) { return true; }
  bool do_find(uint64_t, uint64_t &) { return false; }
  bool do_erase(uint64_t) { return true; }
  void do_clear() {}
  std::string do_name() const { return "FeldmanHashMap(stub)"; }
};

struct CdsThreadGuard {};
inline void cds_global_init() {}
inline void cds_global_terminate() {}

} // namespace bench

#endif // NO_LIBCDS
