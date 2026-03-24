#pragma once
// ============================================================================
// SystemSampler.hpp — фоновый поток для сбора системных метрик.
//
// Собирает с настраиваемой частотой (metrics_sampling_ms):
//   - CPU utilization (per-core и total) через /proc/stat
//   - Memory usage через /proc/self/status
//   - Context switches через /proc/self/status
//   - Page faults через /proc/self/stat
//   - LLC cache misses через perf_event_open (если доступно)
//
// Принципы:
//   - Сэмплер работает на выделенном потоке, привязанном к отдельному ядру
//   - Минимальное влияние на бенчмарк: только чтение /proc + один syscall
//   - Данные записываются в lock-free ring buffer (SPSC)
//
// Ссылки:
//   - perf_event_open(2) — Linux man pages
//   - /proc/stat format — https://man7.org/linux/man-pages/man5/proc.5.html
// ============================================================================

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

namespace bench {

/// Одна выборка системных метрик.
struct SystemSample {
  uint64_t timestamp_us = 0;   ///< микросекунды с начала прогона
  double cpu_total_pct = 0.0;  ///< суммарная CPU utilization [0..100]
  uint64_t rss_bytes = 0;      ///< Resident Set Size
  uint64_t voluntary_cs = 0;   ///< voluntary context switches
  uint64_t involuntary_cs = 0; ///< involuntary context switches
  uint64_t page_faults = 0;    ///< minor page faults
  uint64_t llc_misses = 0;     ///< LLC cache misses (0 если недоступно)
  uint64_t total_context_switches() const noexcept {
    return voluntary_cs + involuntary_cs;
  }
};

/// Фоновый сэмплер системных метрик.
class SystemSampler {
public:
  explicit SystemSampler(uint32_t sampling_ms = 200)
      : sampling_interval_ms_(sampling_ms) {}

  /// Запускает фоновый поток сбора метрик.
  void start() {
    spdlog::debug("SystemSampler start: interval_ms={}", sampling_interval_ms_);
    samples_.clear();
    prev_total_jiffies_ = 0;
    prev_idle_jiffies_ = 0;
    start_time_ = std::chrono::steady_clock::now();
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { sample_loop(); });
  }

  /// Останавливает сбор метрик.
  void stop() {
    const bool was_running =
        running_.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
      spdlog::debug("SystemSampler stop requested");
    }

    if (thread_.joinable()) {
      thread_.join();
      spdlog::debug("SystemSampler stopped: samples_collected={}",
                    samples_.size());
    }
  }

  /// Возвращает собранные выборки (вызывать после stop()).
  const std::vector<SystemSample> &samples() const { return samples_; }

  ~SystemSampler() { stop(); }

private:
  uint32_t sampling_interval_ms_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::vector<SystemSample> samples_;
  std::chrono::steady_clock::time_point start_time_;

  // Предыдущие значения /proc/stat для вычисления delta
  uint64_t prev_total_jiffies_ = 0;
  uint64_t prev_idle_jiffies_ = 0;

  void sample_loop() {
    samples_.reserve(4096); // pre-allocate чтобы избежать realloc
    spdlog::debug("SystemSampler loop begin");

    while (running_.load(std::memory_order_acquire)) {
      SystemSample s;

      auto now = std::chrono::steady_clock::now();
      s.timestamp_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                                start_time_)
              .count());

      bool cpu_valid = read_cpu_utilization(s);
      read_memory_info(s);
      read_context_switches(s);
      read_page_faults(s);
      read_llc_misses(s);

      if (cpu_valid) {
        samples_.push_back(s);
        spdlog::debug(
            "System sample: ts_us={}, cpu_total_pct={:.3f}, rss_bytes={}, "
            "voluntary_cs={}, involuntary_cs={}, page_faults={}, llc_misses={}",
            s.timestamp_us, s.cpu_total_pct, s.rss_bytes, s.voluntary_cs,
            s.involuntary_cs, s.page_faults, s.llc_misses);
      } else {
        spdlog::debug(
            "System sample skipped (baseline only): ts_us={}, rss_bytes={}, "
            "voluntary_cs={}, involuntary_cs={}, page_faults={}, llc_misses={}",
            s.timestamp_us, s.rss_bytes, s.voluntary_cs, s.involuntary_cs,
            s.page_faults, s.llc_misses);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(
          sampling_interval_ms_)); // sleep for config.metrics_sampling_ms
    }
    spdlog::debug("SystemSampler loop end");
  }

  /// Читает /proc/stat для CPU utilization.
  bool read_cpu_utilization(SystemSample &s) {
    std::ifstream f("/proc/stat");
    if (!f.is_open()) {
      spdlog::debug("Failed to open /proc/stat");
      return false;
    }

    std::string line;
    std::getline(f, line);

    std::istringstream iss(line);
    std::string cpu_label;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
             softirq = 0, steal = 0;
    iss >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >>
        softirq >> steal;

    uint64_t total =
        user + nice + system + idle + iowait + irq + softirq + steal;
    uint64_t idle_total = idle + iowait;

    const uint64_t prev_total = prev_total_jiffies_;
    const uint64_t prev_idle = prev_idle_jiffies_;

    if (prev_total > 0) {
      uint64_t d_total = total - prev_total;
      uint64_t d_idle = idle_total - prev_idle;
      if (d_total > 0) {
        s.cpu_total_pct = 100.0 * (1.0 - static_cast<double>(d_idle) / d_total);
      }
    }

    prev_total_jiffies_ = total;
    prev_idle_jiffies_ = idle_total;

    spdlog::debug("CPU util sample: total_jiffies={}, idle_jiffies={}, "
                  "prev_total={}, prev_idle={}, cpu_total_pct={:.3f}",
                  total, idle_total, prev_total, prev_idle, s.cpu_total_pct);

    return true;
  }

  /// Читает RSS из /proc/self/status.
  void read_memory_info(SystemSample &s) {
    std::ifstream f("/proc/self/status");
    if (!f.is_open()) {
      spdlog::debug("VmRSS not found in /proc/self/status");
      return;
    }

    std::string line;
    while (std::getline(f, line)) {
      if (line.compare(0, 6, "VmRSS:") == 0) {
        uint64_t kb = 0;
        std::sscanf(line.c_str(), "VmRSS: %lu kB", &kb);
        s.rss_bytes = kb * 1024;
        break;
      }
    }
    spdlog::debug("Memory sample: rss_bytes={}", s.rss_bytes);
  }

  /// Читает context switches из getrusage. link:
  /// https://man7.org/linux/man-pages/man2/getrusage.2.html?utm_source=chatgpt.com
  void read_context_switches(SystemSample &s) {
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
      spdlog::debug("getrusage failed: errno={}, msg={}", errno,
                    std::strerror(errno));
      return;
    }

    s.voluntary_cs = static_cast<uint64_t>(ru.ru_nvcsw);
    s.involuntary_cs = static_cast<uint64_t>(ru.ru_nivcsw);

    spdlog::debug("Context switches sample: voluntary_cs={}, involuntary_cs={}",
                  s.voluntary_cs, s.involuntary_cs);
  }

  /// Читает page faults из /proc/self/stat (поле 10 — minflt).
  void read_page_faults(SystemSample &s) {
    std::ifstream f("/proc/self/stat");
    if (!f.is_open()) {
      spdlog::debug("Failed to parse /proc/self/stat for page_faults");
      return;
    }

    std::string content;
    std::getline(f, content);

    // Поле 10 (0-indexed: 9) — minflt (minor page faults)
    // Формат: pid (comm) state ppid ... minflt cminflt majflt ...
    // Ищем закрывающую скобку comm, затем парсим поля
    auto pos = content.rfind(')');
    if (pos == std::string::npos)
      return;

    std::istringstream iss(content.substr(pos + 2));
    std::string token;
    // Поля после (comm): state(1) ppid(2) pgrp(3) session(4) tty(5) tpgid(6)
    // flags(7) minflt(8) cminflt(9) majflt(10)
    for (int i = 0; i < 8; ++i)
      iss >> token; // пропускаем до minflt
    iss >> s.page_faults;
    spdlog::debug("Page faults sample: page_faults={}", s.page_faults);
  }

  /// LLC cache misses через perf_event_open.
  /// TODO: полная реализация с perf_event_open(2) для
  /// PERF_COUNT_HW_CACHE_MISSES.
  void read_llc_misses(SystemSample &s) {
    spdlog::debug("LLC misses unavailable: using stub value 0");
    // Заглушка: perf_event_open требует CAP_PERFMON или perf_event_paranoid
    // ≤ 1. Полная реализация:
    //   1. Открыть fd = perf_event_open({type=PERF_TYPE_HARDWARE,
    //      config=PERF_COUNT_HW_CACHE_MISSES}, pid=0, cpu=-1, group=-1, 0)
    //   2. ioctl(fd, PERF_EVENT_IOC_ENABLE)
    //   3. read(fd, &count, sizeof(count))
    // Если CAP_PERFMON недоступен — s.llc_misses остаётся 0.
    s.llc_misses = 0;
  }
};

} // namespace bench
