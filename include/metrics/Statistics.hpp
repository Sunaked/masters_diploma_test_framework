#pragma once
// ============================================================================
// Statistics.hpp — статистический анализ результатов бенчмарка.
//
// Реализует:
//   - mean, stddev, min, max
//   - Процентили (p50, p95, p99, p99.9) через std::nth_element
//   - 95% доверительный интервал по t-распределению Стьюдента
//
// Доверительный интервал важен для научной воспроизводимости: он показывает,
// насколько стабильны результаты при повторных запусках.
//
// Ссылки:
//   [5] Montgomery, "Design and Analysis of Experiments", 9th ed.
//   [6] Student's t-distribution — критические значения для малых выборок
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace bench {

/// Результат статистического анализа одной метрики.
struct StatResult {
    double mean    = 0.0;
    double stddev  = 0.0;
    double min_val = 0.0;
    double max_val = 0.0;
    double ci95_lo = 0.0;   ///< нижняя граница 95% CI
    double ci95_hi = 0.0;   ///< верхняя граница 95% CI
    double p50     = 0.0;
    double p95     = 0.0;
    double p99     = 0.0;
    double p999    = 0.0;
};

/// Критические значения t-распределения для 95% CI (two-tailed, α=0.05).
/// Индекс = degrees_of_freedom - 1 (для df от 1 до 30).
/// Для df > 30 используем приближение z ≈ 1.96.
inline double t_critical_95(size_t df) {
    // Таблица для df = 1..30 (стандартные значения)
    static constexpr double table[] = {
        12.706, 4.303, 3.182, 2.776, 2.571,  // df 1-5
         2.447, 2.365, 2.306, 2.262, 2.228,  // df 6-10
         2.201, 2.179, 2.160, 2.145, 2.131,  // df 11-15
         2.120, 2.110, 2.101, 2.093, 2.086,  // df 16-20
         2.080, 2.074, 2.069, 2.064, 2.060,  // df 21-25
         2.056, 2.052, 2.048, 2.045, 2.042,  // df 26-30
    };
    if (df == 0) return 12.706;
    if (df <= 30) return table[df - 1];
    return 1.96; // нормальное приближение для больших df
}

/// Вычисляет полную статистику по вектору наблюдений.
inline StatResult compute_stats(std::vector<double> data) {
    StatResult r;
    if (data.empty()) return r;

    const size_t n = data.size();

    // mean
    r.mean = std::accumulate(data.begin(), data.end(), 0.0) / n;

    // min / max
    auto [it_min, it_max] = std::minmax_element(data.begin(), data.end());
    r.min_val = *it_min;
    r.max_val = *it_max;

    // stddev (sample, Bessel's correction)
    if (n > 1) {
        double sum_sq = 0.0;
        for (double x : data) {
            double d = x - r.mean;
            sum_sq += d * d;
        }
        r.stddev = std::sqrt(sum_sq / (n - 1));

        // 95% доверительный интервал: mean ± t * (stddev / sqrt(n))
        double t = t_critical_95(n - 1);
        double margin = t * r.stddev / std::sqrt(static_cast<double>(n));
        r.ci95_lo = r.mean - margin;
        r.ci95_hi = r.mean + margin;
    } else {
        r.ci95_lo = r.mean;
        r.ci95_hi = r.mean;
    }

    // Процентили через partial_sort / nth_element
    auto percentile = [&](double p) -> double {
        if (n == 1) return data[0];
        double idx = p * (n - 1);
        size_t lo = static_cast<size_t>(std::floor(idx));
        size_t hi = std::min(lo + 1, n - 1);
        double frac = idx - lo;

        // nth_element для O(n) вместо O(n log n) сортировки
        std::nth_element(data.begin(), data.begin() + lo, data.end());
        double v_lo = data[lo];
        if (lo != hi) {
            std::nth_element(data.begin(), data.begin() + hi, data.end());
        }
        double v_hi = data[hi];
        return v_lo + frac * (v_hi - v_lo);
    };

    r.p50  = percentile(0.50);
    r.p95  = percentile(0.95);
    r.p99  = percentile(0.99);
    r.p999 = percentile(0.999);

    return r;
}

} // namespace bench
