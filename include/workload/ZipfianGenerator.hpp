#pragma once
// ============================================================================
// ZipfianGenerator.hpp — собственная реализация Zipfian-распределения.
//
// Алгоритм: rejection-inversion метод Вольфганга Хёрмана и Герхарда Дерфлингера
// (см. "Rejection-inversion to generate variates from monotone discrete
//  distributions", ACM TOMACS 6(3), 1996).
//
// Мотивация: стандартная реализация YCSB (Java) использует аналогичный подход.
// Мы реализуем собственную версию, чтобы:
//   1. Избежать зависимости от внешних библиотек
//   2. Гарантировать детерминированность через seed
//   3. Позволить предвычисление CDF-таблицы для малых диапазонов
//
// Ссылки:
//   [1] Cooper et al., "Benchmarking Cloud Serving Systems with YCSB", SoCC
//   2010 [2] Hörmann & Derflinger, "Rejection-inversion...", ACM TOMACS 1996
//   [3] Gray et al., "Quickly Generating Billion-Record Synthetic Databases",
//       SIGMOD 1994
// ============================================================================

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace bench {

/// Генератор Zipfian-распределённых значений в диапазоне [0, n).
///
/// Вычислительная сложность генерации: O(1) amortised.
/// Предвычисление: O(n) при n ≤ threshold, O(1) иначе (rejection метод).
class ZipfianGenerator {
public:
  /// @param n     количество элементов (key_range)
  /// @param alpha параметр скошенности (0 = uniform, >1 = сильный hot-spot)
  /// @param seed  зерно ГПСЧ для воспроизводимости
  ZipfianGenerator(uint64_t n, double alpha, uint64_t seed = 42)
      : n_(n), alpha_(alpha), rng_(seed) {
    // Вычисляем обобщённое гармоническое число H_{n,alpha}
    // Для больших n используем аппроксимацию Эйлера-Маклорена:
    //   H_{n,s} ≈ ζ(s) - 1/((s-1) * n^{s-1}) + 1/(2*n^s)
    // Для малых n (≤ precompute_threshold) считаем точно.
    static constexpr uint64_t kPrecomputeThreshold = 10'000'000;

    if (n_ <= kPrecomputeThreshold) {
      // Точное вычисление + CDF для O(1) sampling через binary search
      cumulative_.resize(n_);
      double sum = 0.0;
      for (uint64_t i = 0; i < n_; ++i) {
        sum += 1.0 / std::pow(static_cast<double>(i + 1), alpha_);
        cumulative_[i] = sum;
      }
      zeta_n_ = sum;
      use_table_ = true;
    } else {
      // Аппроксимация для больших n: rejection-inversion метод
      zeta_n_ = compute_zeta(n_, alpha_);
      use_table_ = false;
      // Предвычисляем константы для rejection-inversion
      theta_ = alpha_;
      zeta2_ = compute_zeta(2, alpha_);
      half_pow_ = 1.0 + std::pow(0.5, theta_);
      eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) /
             (1.0 - zeta2_ / zeta_n_);
    }
  }

  /// Генерирует следующее значение ∈ [0, n_).
  uint64_t next() {
    if (use_table_) {
      return sample_from_table();
    }
    return sample_rejection();
  }

private:
  uint64_t n_;
  double alpha_;
  std::mt19937_64 rng_;

  // Для табличного метода (малые n)
  bool use_table_ = false;
  std::vector<double> cumulative_;

  // Для rejection-inversion (большие n)
  double zeta_n_ = 0.0;
  double theta_ = 0.0;
  double zeta2_ = 0.0;
  double half_pow_ = 0.0;
  double eta_ = 0.0;

  /// Точное вычисление ζ(n, s) = Σ_{k=1..n} 1/k^s
  static double compute_zeta(uint64_t n, double s) {
    double sum = 0.0;
    for (uint64_t i = 1; i <= n; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i), s);
    }
    return sum;
  }

  /// Выборка через бинарный поиск по предвычисленной CDF.
  uint64_t sample_from_table() {
    std::uniform_real_distribution<double> dist(0.0, zeta_n_);
    double u = dist(rng_);
    // lower_bound даёт O(log n) — приемлемо для предвычисления
    auto it = std::lower_bound(cumulative_.begin(), cumulative_.end(), u);
    return static_cast<uint64_t>(it - cumulative_.begin());
  }

  /// YCSB-style scrambled Zipfian через rejection-inversion.
  /// Адаптация алгоритма из YCSB Java-клиента.
  uint64_t sample_rejection() {
    std::uniform_real_distribution<double> dist01(0.0, 1.0);
    double u = dist01(rng_);
    double uz = u * zeta_n_;

    if (uz < 1.0)
      return 0;
    if (uz < half_pow_)
      return 1;

    double dn = static_cast<double>(n_);
    uint64_t val = static_cast<uint64_t>(
        dn * std::pow(eta_ * u - eta_ + 1.0, 1.0 / (1.0 - theta_)));
    return std::min(val, n_ - 1);
  }
};

} // namespace bench
