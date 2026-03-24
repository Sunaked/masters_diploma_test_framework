#pragma once
// ============================================================================
// ReservoirSampler.hpp — fixed-memory latency sampling via Algorithm R.
//
// Algorithm R (Vitter, 1985): maintains a uniform random sample of size k
// from a stream of unknown length. O(1) per operation, O(k) memory.
//
// This eliminates linear RSS growth with workload size. For 140M ops at
// 8 bytes each, the old approach needed ~1.1 GB per thread just for latencies.
// With reservoir_capacity=1'000'000 we use ~8 MB per thread — a 137x reduction.
//
// References:
//   Vitter, "Random Sampling with a Reservoir", ACM TOMS 11(1), 1985.
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace bench {

// Thread-affine: one sampler per thread, no synchronization needed.
struct alignas(64) ReservoirSampler {
  static constexpr size_t kDefaultCapacity = 1'000'000;

  std::vector<uint64_t> samples;
  uint64_t total_count = 0; // total observations seen (for mean/throughput)
  uint64_t total_sum = 0;   // running sum for mean computation
  size_t capacity = kDefaultCapacity;

  std::mt19937_64 rng;

  // Initialize with given capacity and seed.
  void reset(size_t cap, uint64_t seed) {
    capacity = cap;
    samples.clear();
    samples.reserve(capacity);
    total_count = 0;
    total_sum = 0;
    rng.seed(seed);
  }

  // Add one observation. O(1) amortized.
  // Hot path: keep branch-free as possible.
  void add(uint64_t value) {
    total_sum += value;
    if (total_count < capacity) {
      samples.push_back(value);
    } else {
      // Algorithm R: replace element j with probability capacity/total_count
      std::uniform_int_distribution<uint64_t> dist(0, total_count);
      uint64_t j = dist(rng);
      if (j < capacity) {
        samples[j] = value;
      }
    }
    ++total_count;
  }

  // Get approximate mean from running sum (exact).
  double mean() const {
    return total_count > 0 ? static_cast<double>(total_sum) /
                                 static_cast<double>(total_count)
                           : 0.0;
  }

  // Check if sampler is empty.
  bool empty() const { return samples.empty(); }

  // Number of retained samples (≤ capacity).
  size_t size() const { return samples.size(); }
};

} // namespace bench
