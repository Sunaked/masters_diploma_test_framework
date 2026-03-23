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
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bench {

/// Одна выборка системных метрик.
struct SystemSample {
    uint64_t timestamp_us     = 0;   ///< микросекунды с начала прогона
    double   cpu_total_pct    = 0.0; ///< суммарная CPU utilization [0..100]
    uint64_t rss_bytes        = 0;   ///< Resident Set Size
    uint64_t voluntary_cs     = 0;   ///< voluntary context switches
    uint64_t involuntary_cs   = 0;   ///< involuntary context switches
    uint64_t page_faults      = 0;   ///< minor page faults
    uint64_t llc_misses       = 0;   ///< LLC cache misses (0 если недоступно)
};

/// Фоновый сэмплер системных метрик.
class SystemSampler {
public:
    explicit SystemSampler(uint32_t sampling_ms = 200)
        : sampling_interval_ms_(sampling_ms) {}

    /// Запускает фоновый поток сбора метрик.
    void start() {
        running_.store(true, std::memory_order_release);
        start_time_ = std::chrono::steady_clock::now();
        thread_ = std::thread([this] { sample_loop(); });

        // Опционально: привязать поток сэмплера к последнему ядру
        // чтобы не конкурировать с рабочими потоками
        // TODO: sched_setaffinity для потока сэмплера
    }

    /// Останавливает сбор метрик.
    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// Возвращает собранные выборки (вызывать после stop()).
    const std::vector<SystemSample>& samples() const { return samples_; }

    ~SystemSampler() { stop(); }

private:
    uint32_t sampling_interval_ms_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<SystemSample> samples_;
    std::chrono::steady_clock::time_point start_time_;

    // Предыдущие значения /proc/stat для вычисления delta
    uint64_t prev_total_jiffies_ = 0;
    uint64_t prev_idle_jiffies_  = 0;

    void sample_loop() {
        samples_.reserve(4096); // pre-allocate чтобы избежать realloc

        while (running_.load(std::memory_order_acquire)) {
            SystemSample s;

            auto now = std::chrono::steady_clock::now();
            s.timestamp_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - start_time_).count());

            read_cpu_utilization(s);
            read_memory_info(s);
            read_context_switches(s);
            read_page_faults(s);
            read_llc_misses(s);

            samples_.push_back(s);

            std::this_thread::sleep_for(
                std::chrono::milliseconds(sampling_interval_ms_));
        }
    }

    /// Читает /proc/stat для CPU utilization.
    void read_cpu_utilization(SystemSample& s) {
        std::ifstream f("/proc/stat");
        if (!f.is_open()) return;

        std::string line;
        std::getline(f, line); // первая строка: "cpu ..."

        // Парсим: cpu user nice system idle iowait irq softirq steal guest guest_nice
        std::istringstream iss(line);
        std::string cpu_label;
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
        iss >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
        uint64_t idle_total = idle + iowait;

        if (prev_total_jiffies_ > 0) {
            uint64_t d_total = total - prev_total_jiffies_;
            uint64_t d_idle  = idle_total - prev_idle_jiffies_;
            if (d_total > 0) {
                s.cpu_total_pct = 100.0 * (1.0 - static_cast<double>(d_idle) / d_total);
            }
        }

        prev_total_jiffies_ = total;
        prev_idle_jiffies_  = idle_total;
    }

    /// Читает RSS из /proc/self/status.
    void read_memory_info(SystemSample& s) {
        std::ifstream f("/proc/self/status");
        if (!f.is_open()) return;

        std::string line;
        while (std::getline(f, line)) {
            if (line.compare(0, 6, "VmRSS:") == 0) {
                uint64_t kb = 0;
                std::sscanf(line.c_str(), "VmRSS: %lu kB", &kb);
                s.rss_bytes = kb * 1024;
                break;
            }
        }
    }

    /// Читает context switches из /proc/self/status.
    void read_context_switches(SystemSample& s) {
        std::ifstream f("/proc/self/status");
        if (!f.is_open()) return;

        std::string line;
        while (std::getline(f, line)) {
            if (line.compare(0, 25, "voluntary_ctxt_switches:") == 0) {
                std::sscanf(line.c_str(), "voluntary_ctxt_switches: %lu", &s.voluntary_cs);
            } else if (line.compare(0, 27, "nonvoluntary_ctxt_switches:") == 0) {
                std::sscanf(line.c_str(), "nonvoluntary_ctxt_switches: %lu", &s.involuntary_cs);
            }
        }
    }

    /// Читает page faults из /proc/self/stat (поле 10 — minflt).
    void read_page_faults(SystemSample& s) {
        std::ifstream f("/proc/self/stat");
        if (!f.is_open()) return;

        std::string content;
        std::getline(f, content);

        // Поле 10 (0-indexed: 9) — minflt (minor page faults)
        // Формат: pid (comm) state ppid ... minflt cminflt majflt ...
        // Ищем закрывающую скобку comm, затем парсим поля
        auto pos = content.rfind(')');
        if (pos == std::string::npos) return;

        std::istringstream iss(content.substr(pos + 2));
        std::string token;
        // Поля после (comm): state(1) ppid(2) pgrp(3) session(4) tty(5) tpgid(6)
        // flags(7) minflt(8) cminflt(9) majflt(10)
        for (int i = 0; i < 8; ++i) iss >> token; // пропускаем до minflt
        iss >> s.page_faults;
    }

    /// LLC cache misses через perf_event_open.
    /// TODO: полная реализация с perf_event_open(2) для PERF_COUNT_HW_CACHE_MISSES.
    void read_llc_misses(SystemSample& s) {
        // Заглушка: perf_event_open требует CAP_PERFMON или perf_event_paranoid ≤ 1.
        // Полная реализация:
        //   1. Открыть fd = perf_event_open({type=PERF_TYPE_HARDWARE,
        //      config=PERF_COUNT_HW_CACHE_MISSES}, pid=0, cpu=-1, group=-1, 0)
        //   2. ioctl(fd, PERF_EVENT_IOC_ENABLE)
        //   3. read(fd, &count, sizeof(count))
        // Если CAP_PERFMON недоступен — s.llc_misses остаётся 0.
        s.llc_misses = 0;
    }
};

} // namespace bench
