#pragma once
// ============================================================================
// Statistics.hpp — статистический анализ результатов бенчмарка.
//
// ============================================================================

#include "ReservoirSampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace bench {

struct StatResult {
  double mean = 0.0;
  double stddev = 0.0;
  double min_val = 0.0;
  double max_val = 0.0;
  double ci95_lo = 0.0;
  double ci95_hi = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
};
// Критические значения t-распределения для 95% CI (two-tailed, α=0.05).
// Индекс = degrees_of_freedom - 1 (для df от 1 до 30).
// Для df > 30 используем приближение z ≈ 1.96.
inline double t_critical_95(size_t df) {
  static constexpr double table[] = {
      12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
      2.201,  2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
      2.080,  2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
  };
  if (df == 0)
    return 12.706;
  if (df <= 30)
    return table[df - 1];
  return 1.96;
}

// Original compute_stats — backward compatible, works on vector<double>.
inline StatResult compute_stats(std::vector<double> data) {
  StatResult r;
  if (data.empty())
    return r;

  const size_t n = data.size();
  r.mean = std::accumulate(data.begin(), data.end(), 0.0) / n;

  auto [it_min, it_max] = std::minmax_element(data.begin(), data.end());
  r.min_val = *it_min;
  r.max_val = *it_max;

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

  auto percentile = [&](double p) -> double {
    if (n == 1)
      return data[0];
    double idx = p * (n - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = std::min(lo + 1, n - 1);
    double frac = idx - lo;
    std::nth_element(data.begin(), data.begin() + lo, data.end());
    double v_lo = data[lo];
    if (lo != hi) {
      std::nth_element(data.begin(), data.begin() + hi, data.end());
    }
    double v_hi = data[hi];
    return v_lo + frac * (v_hi - v_lo);
  };

  r.p50 = percentile(0.50);
  r.p95 = percentile(0.95);
  r.p99 = percentile(0.99);
  r.p999 = percentile(0.999);

  return r;
}

// Compute stats from a ReservoirSampler without copying to vector<double>.
// Uses the sampler's exact mean (from running sum) and percentiles from
// the reservoir sample. The reservoir provides unbiased percentile estimates.
inline StatResult compute_stats_from_reservoir(ReservoirSampler &sampler) {
  StatResult r;
  if (sampler.empty())
    return r;

  // Use exact mean from running sum (not sampled)
  r.mean = sampler.mean();

  auto &data = sampler.samples;
  const size_t n = data.size();

  auto [it_min, it_max] = std::minmax_element(data.begin(), data.end());
  r.min_val = static_cast<double>(*it_min);
  r.max_val = static_cast<double>(*it_max);

  // stddev from sample
  if (n > 1) {
    double sum_sq = 0.0;
    for (uint64_t x : data) {
      double d = static_cast<double>(x) - r.mean;
      sum_sq += d * d;
    }
    r.stddev = std::sqrt(sum_sq / (n - 1));
    double t = t_critical_95(n - 1);
    double margin = t * r.stddev / std::sqrt(static_cast<double>(n));
    r.ci95_lo = r.mean - margin;
    r.ci95_hi = r.mean + margin;
  } else {
    r.ci95_lo = r.mean;
    r.ci95_hi = r.mean;
  }

  // Percentiles via nth_element on the reservoir (in-place, O(n))
  auto percentile = [&](double p) -> double {
    if (n == 1)
      return static_cast<double>(data[0]);
    double idx = p * (n - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = std::min(lo + 1, n - 1);
    double frac = idx - lo;
    std::nth_element(data.begin(), data.begin() + lo, data.end());
    double v_lo = static_cast<double>(data[lo]);
    if (lo != hi) {
      std::nth_element(data.begin(), data.begin() + hi, data.end());
    }
    double v_hi = static_cast<double>(data[hi]);
    return v_lo + frac * (v_hi - v_lo);
  };

  r.p50 = percentile(0.50);
  r.p95 = percentile(0.95);
  r.p99 = percentile(0.99);
  r.p999 = percentile(0.999);

  return r;
}

// Merge multiple ReservoirSamplers into one combined result.
// Concatenates samples (up to combined capacity) and sums counts.
inline StatResult
compute_stats_from_reservoirs(std::vector<ReservoirSampler> &samplers) {
  // Build a merged sampler: concatenate all samples, use combined sum/count
  ReservoirSampler merged;
  uint64_t total_sum = 0;
  uint64_t total_count = 0;
  size_t total_samples = 0;

  for (auto &s : samplers) {
    total_sum += s.total_sum;
    total_count += s.total_count;
    total_samples += s.samples.size();
  }

  merged.total_sum = total_sum;
  merged.total_count = total_count;

  // If total samples fit in default capacity, use all; else subsample
  if (total_samples <= ReservoirSampler::kDefaultCapacity) {
    merged.samples.reserve(total_samples);
    for (auto &s : samplers) {
      merged.samples.insert(merged.samples.end(), s.samples.begin(),
                            s.samples.end());
    }
  } else {
    // Weighted reservoir merge: take proportional samples from each
    merged.samples.reserve(ReservoirSampler::kDefaultCapacity);
    std::mt19937_64 rng(42);
    for (auto &s : samplers) {
      size_t take = static_cast<size_t>(static_cast<double>(s.samples.size()) /
                                        static_cast<double>(total_samples) *
                                        ReservoirSampler::kDefaultCapacity);
      take = std::min(take, s.samples.size());
      // Shuffle and take first 'take'
      std::shuffle(s.samples.begin(), s.samples.end(), rng);
      merged.samples.insert(merged.samples.end(), s.samples.begin(),
                            s.samples.begin() + take);
    }
  }

  return compute_stats_from_reservoir(merged);
}
} // namespace bench
