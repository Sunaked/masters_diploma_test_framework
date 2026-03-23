#pragma once
// ============================================================================
// WorkloadGenerator.hpp — предвычисление операций для бенчмарка.
//
// Ключевой инвариант: ВСЕ аллокации и генерация случайных чисел выполняются
// ДО старта замера. Горячий цикл итерирует по std::vector<Op> — нулевые
// накладные расходы помимо cache misses при чтении вектора.
//
// Стратегия предвычисления:
//   1. Для каждой фазы оцениваем количество операций как:
//      estimated_ops = duration_ms * estimated_throughput_per_ms
//      (conservative estimate: 1M ops/ms * thread_count)
//   2. Генерируем вектор Op для каждого потока отдельно, чтобы избежать
//      false sharing при чтении.
//   3. Каждый поток получает свой seed = base_seed + thread_id для
//      воспроизводимости.
//
// Ссылки:
//   [1] YCSB workload spec: https://github.com/brianfrankcooper/YCSB/wiki
// ============================================================================

#include "Phase.hpp"
#include "ZipfianGenerator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

namespace bench {

/// Параметры предвычисления.
struct WorkloadConfig {
    /// Сколько операций генерировать на 1 мс длительности фазы (на поток).
    /// Консервативная оценка: если контейнер быстрее — поток просто дойдёт
    /// до конца вектора раньше таймаута; если медленнее — фаза завершится
    /// по таймеру, а оставшиеся операции не будут выполнены.
    uint64_t ops_per_ms_per_thread = 500'000;

    /// Базовый seed для ГПСЧ. Поток i получит seed = base_seed + i.
    uint64_t base_seed = 12345;
};

/// Генерирует предвычисленный набор операций для одного потока и одной фазы.
inline std::vector<Op> generate_phase_ops(
    const Phase& phase,
    uint64_t     thread_id,
    uint64_t     ops_per_ms_per_thread,
    uint64_t     base_seed)
{
    const uint64_t total_ops = phase.duration_ms * ops_per_ms_per_thread;
    std::vector<Op> ops;
    ops.reserve(total_ops);

    // Seed: уникален для (phase, thread) → воспроизводимость
    const uint64_t seed = base_seed ^ (thread_id * 2654435761ULL);

    // --- генератор ключей ---
    std::mt19937_64 rng(seed);
    std::unique_ptr<ZipfianGenerator> zipf;

    if (phase.key_dist == KeyDist::Zipfian) {
        zipf = std::make_unique<ZipfianGenerator>(phase.key_range, phase.alpha, seed + 1);
    }

    std::uniform_int_distribution<uint64_t> uniform_key(0, phase.key_range - 1);
    uint64_t sequential_counter = thread_id; // каждый поток стартует со своего offset

    // --- генератор типа операции ---
    // Используем дискретное распределение через пороги для O(1) выбора.
    // insert_ratio + find_ratio + erase_ratio ≤ 1.0
    const double threshold_insert = phase.insert_ratio;
    const double threshold_find   = phase.insert_ratio + phase.find_ratio;
    std::uniform_real_distribution<double> op_dist(0.0, 1.0);

    for (uint64_t i = 0; i < total_ops; ++i) {
        Op op{};

        // Тип операции
        double r = op_dist(rng);
        if (r < threshold_insert) {
            op.type = OpType::Insert;
        } else if (r < threshold_find) {
            op.type = OpType::Find;
        } else {
            op.type = OpType::Erase;
        }

        // Ключ
        switch (phase.key_dist) {
        case KeyDist::Uniform:
            op.key = uniform_key(rng);
            break;
        case KeyDist::Zipfian:
            op.key = zipf->next();
            break;
        case KeyDist::Sequential:
            op.key = sequential_counter;
            sequential_counter += 1; // можно += thread_count для interleaving
            break;
        }

        ops.push_back(op);
    }

    return ops;
}

/// Генерирует наборы операций для ВСЕХ потоков и ВСЕХ фаз сценария.
/// Возвращает: ops[phase_index][thread_id] = vector<Op>
inline std::vector<std::vector<std::vector<Op>>> generate_scenario_ops(
    const Scenario&       scenario,
    uint32_t              thread_count,
    const WorkloadConfig& config = {})
{
    std::vector<std::vector<std::vector<Op>>> all_ops;
    all_ops.reserve(scenario.phases.size());

    for (size_t pi = 0; pi < scenario.phases.size(); ++pi) {
        const auto& phase = scenario.phases[pi];
        std::vector<std::vector<Op>> phase_ops(thread_count);

        for (uint32_t tid = 0; tid < thread_count; ++tid) {
            // Уникальный seed для каждой (фаза, поток) комбинации
            uint64_t phase_seed = config.base_seed + pi * 1'000'000 + tid;
            phase_ops[tid] = generate_phase_ops(
                phase, tid, config.ops_per_ms_per_thread, phase_seed);
        }

        all_ops.push_back(std::move(phase_ops));
    }

    return all_ops;
}

} // namespace bench
