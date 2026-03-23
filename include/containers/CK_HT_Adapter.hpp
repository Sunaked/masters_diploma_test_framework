#pragma once
// ============================================================================
// CK_HT_Adapter.hpp — адаптер для Concurrency Kit ck_ht (hash table).
//
// ck_ht — высокопроизводительная lock-free хеш-таблица из библиотеки
// Concurrency Kit (Samy Al Bahra). Использует read-mostly оптимизации
// и epoch-based reclamation.
//
// ВАЖНО: ck_ht — C API. Этот адаптер оборачивает его в C++ интерфейс.
// ck_ht не thread-safe для одновременных writer'ов без внешней синхронизации;
// для concurrent writes используется ck_ht с CK_HT_MODE_BYTESTRING и
// внешний RWLock (или CK spinlock). Для read-heavy workloads это приемлемо.
//
// Альтернатива: ck_hs (set) — но нам нужен map (key→value).
//
// Ссылки:
//   [3] https://github.com/concurrencykit/ck
//   [4] Al Bahra, "Nonblocking Algorithms and Scalable Multicore Programming",
//       CACM 56(7), 2013
// ============================================================================

#include "ContainerAdapter.hpp"

#ifndef NO_CK

#include <ck_ht.h>
#include <ck_spinlock.h>
#include <cstdlib>
#include <cstring>

namespace bench {

/// Аллокатор для ck_ht (malloc/free wrapper, требуемый CK API).
namespace detail {
    static void* ck_malloc(size_t sz) { return std::malloc(sz); }
    static void  ck_free(void* p, [[maybe_unused]] size_t sz, [[maybe_unused]] bool defer) { std::free(p); }
    // Hash функция для uint64_t ключей (murmur-подобная).
    static uint64_t ck_hash_u64(const void* key, [[maybe_unused]] uint64_t key_length,
                                 [[maybe_unused]] uint64_t seed) {
        uint64_t k;
        std::memcpy(&k, key, sizeof(k));
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }
} // namespace detail

class CK_HT_Adapter : public ContainerCRTP<CK_HT_Adapter> {
public:
    CK_HT_Adapter() {
        ck_ht_allocator alloc = { detail::ck_malloc, detail::ck_free };
        // MODE_BYTESTRING: ключи и значения — произвольные байтовые строки.
        // Начальная ёмкость 1<<16 записей.
        ck_ht_init(&ht_, CK_HT_MODE_BYTESTRING, detail::ck_hash_u64,
                   &alloc, 1ULL << 16, /*seed=*/42);
        ck_spinlock_init(&lock_);
    }

    ~CK_HT_Adapter() {
        ck_ht_destroy(&ht_);
    }

    bool do_insert(uint64_t key, uint64_t value) {
        ck_ht_entry_t entry;
        ck_ht_hash_t  h;

        ck_ht_hash(&h, &ht_, &key, sizeof(key));
        ck_ht_entry_set(&entry, h, &key, sizeof(key), &value, sizeof(value));

        // ck_ht_put не thread-safe для concurrent writers → spinlock
        ck_spinlock_lock(&lock_);
        bool ok = ck_ht_put_spmc(&ht_, h, &entry);
        ck_spinlock_unlock(&lock_);
        return ok;
    }

    bool do_find(uint64_t key, uint64_t& value) {
        ck_ht_entry_t entry;
        ck_ht_hash_t  h;

        ck_ht_hash(&h, &ht_, &key, sizeof(key));
        ck_ht_entry_key_set(&entry, &key, sizeof(key));

        // ck_ht_get_spmc — lock-free для readers
        if (ck_ht_get_spmc(&ht_, h, &entry)) {
            std::memcpy(&value, ck_ht_entry_value(&entry), sizeof(value));
            return true;
        }
        return false;
    }

    bool do_erase(uint64_t key) {
        ck_ht_entry_t entry;
        ck_ht_hash_t  h;

        ck_ht_hash(&h, &ht_, &key, sizeof(key));
        ck_ht_entry_key_set(&entry, &key, sizeof(key));

        ck_spinlock_lock(&lock_);
        bool ok = ck_ht_remove_spmc(&ht_, h, &entry);
        ck_spinlock_unlock(&lock_);
        return ok;
    }

    void do_clear() {
        ck_spinlock_lock(&lock_);
        ck_ht_reset_spmc(&ht_);
        ck_spinlock_unlock(&lock_);
    }

    std::string do_name() const { return "CK_HT"; }

private:
    ck_ht_t        ht_;
    ck_spinlock_t  lock_;
};

} // namespace bench

#else // NO_CK — заглушка

namespace bench {

class CK_HT_Adapter : public ContainerCRTP<CK_HT_Adapter> {
public:
    bool do_insert(uint64_t, uint64_t) { return true; }
    bool do_find(uint64_t, uint64_t&)  { return false; }
    bool do_erase(uint64_t)            { return true; }
    void do_clear() {}
    std::string do_name() const { return "CK_HT(stub)"; }
};

} // namespace bench

#endif // NO_CK
