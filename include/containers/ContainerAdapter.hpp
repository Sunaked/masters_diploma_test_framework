#pragma once
// ============================================================================
// ContainerAdapter.hpp — единый интерфейс для всех тестируемых контейнеров.
//
// ============================================================================

#include <cstdint>
#include <memory>
#include <string>

namespace bench {

// ---------------------------------------------------------------------------
// CRTP base: статический полиморфизм для горячего пути
// ---------------------------------------------------------------------------

template <typename Derived> class ContainerCRTP {
public:
  bool insert(uint64_t key, uint64_t value) {
    return static_cast<Derived *>(this)->do_insert(key, value);
  }
  bool find(uint64_t key, uint64_t &value) {
    return static_cast<Derived *>(this)->do_find(key, value);
  }
  bool erase(uint64_t key) {
    return static_cast<Derived *>(this)->do_erase(key);
  }
  void clear() { static_cast<Derived *>(this)->do_clear(); }
  std::string name() const {
    return static_cast<const Derived *>(this)->do_name();
  }
};

// ---------------------------------------------------------------------------
// Virtual base: для динамического диспатча при создании из конфигурации
// ---------------------------------------------------------------------------

class ContainerBase {
public:
  virtual ~ContainerBase() = default;
  virtual bool insert(uint64_t key, uint64_t value) = 0;
  virtual bool find(uint64_t key, uint64_t &value) = 0;
  virtual bool erase(uint64_t key) = 0;
  virtual void clear() = 0;
  virtual std::string name() const = 0;
};

template <typename Derived> class ContainerBridge : public ContainerBase {
public:
  bool insert(uint64_t key, uint64_t value) override {
    return impl_.insert(key, value);
  }
  bool find(uint64_t key, uint64_t &value) override {
    return impl_.find(key, value);
  }
  bool erase(uint64_t key) override { return impl_.erase(key); }
  void clear() override { impl_.clear(); }
  std::string name() const override { return impl_.name(); }
  Derived &get() { return impl_; }

protected:
  Derived impl_;
};

using ContainerFactory = std::unique_ptr<ContainerBase> (*)();

// ---------------------------------------------------------------------------
// NullContainer — measures pure harness overhead (loop, rdtsc, sampling).
// All operations are no-ops. Subtract NullContainer latency from real
// container latency to isolate true container cost.
// ---------------------------------------------------------------------------

class NullContainer : public ContainerBase {
public:
  bool insert(uint64_t /*key*/, uint64_t /*value*/) override { return true; }
  bool find(uint64_t /*key*/, uint64_t &value) override {
    value = 0;
    return true;
  }
  bool erase(uint64_t /*key*/) override { return true; }
  void clear() override {}
  std::string name() const override { return "NullContainer"; }
};
} // namespace bench
