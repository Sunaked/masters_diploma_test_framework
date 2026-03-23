#pragma once
// ============================================================================
// CsvWriter.hpp — вывод результатов бенчмарка в CSV.
//
// Два файла:
//   1. raw_results.csv — каждая строка = один прогон (run)
//   2. summary.csv — агрегированные статистики по контейнерам
//
// Формат CSV выбран для совместимости с pandas, R, gnuplot и Excel.
// ============================================================================

#include "metrics/Statistics.hpp"
#include "metrics/SystemSampler.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bench {

/// Результат одного прогона (run).
struct RunResult {
    std::string container_name;
    std::string scenario_name;
    uint32_t    thread_count  = 0;
    uint32_t    run_index     = 0;

    // Throughput
    uint64_t total_ops       = 0;
    double   duration_sec    = 0.0;
    double   throughput_ops  = 0.0; ///< ops/sec

    // Latency (наносекунды или тики rdtsc)
    double lat_p50  = 0.0;
    double lat_p95  = 0.0;
    double lat_p99  = 0.0;
    double lat_p999 = 0.0;

    // Системные метрики (средние за прогон)
    double avg_cpu_pct    = 0.0;
    double avg_rss_mb     = 0.0;
    double avg_cs_per_sec = 0.0;
};

/// Запись в summary (агрегация по контейнеру × сценарий × threads).
struct SummaryEntry {
    std::string container_name;
    std::string scenario_name;
    uint32_t    thread_count = 0;
    uint32_t    num_runs     = 0;

    StatResult  throughput_stats;
    StatResult  lat_p50_stats;
    StatResult  lat_p99_stats;
};

class CsvWriter {
public:
    explicit CsvWriter(const std::string& output_dir = "results") {
        std::filesystem::create_directories(output_dir);
        raw_path_     = output_dir + "/raw_results.csv";
        summary_path_ = output_dir + "/summary.csv";
        system_path_  = output_dir + "/system_metrics.csv";
    }

    /// Записывает заголовок raw CSV (вызвать один раз).
    void write_raw_header() {
        std::ofstream f(raw_path_, std::ios::trunc);
        f << "container,scenario,threads,run,"
             "total_ops,duration_sec,throughput_ops_sec,"
             "lat_p50_ns,lat_p95_ns,lat_p99_ns,lat_p999_ns,"
             "avg_cpu_pct,avg_rss_mb,avg_cs_per_sec\n";
    }

    /// Дописывает одну строку в raw CSV.
    void append_raw(const RunResult& r) {
        std::ofstream f(raw_path_, std::ios::app);
        f << r.container_name << ","
          << r.scenario_name << ","
          << r.thread_count << ","
          << r.run_index << ","
          << r.total_ops << ","
          << r.duration_sec << ","
          << r.throughput_ops << ","
          << r.lat_p50 << ","
          << r.lat_p95 << ","
          << r.lat_p99 << ","
          << r.lat_p999 << ","
          << r.avg_cpu_pct << ","
          << r.avg_rss_mb << ","
          << r.avg_cs_per_sec << "\n";
    }

    /// Записывает summary CSV (вызвать один раз, после всех прогонов).
    void write_summary(const std::vector<SummaryEntry>& entries) {
        std::ofstream f(summary_path_, std::ios::trunc);
        f << "container,scenario,threads,num_runs,"
             "throughput_mean,throughput_stddev,throughput_ci95_lo,throughput_ci95_hi,"
             "throughput_min,throughput_max,"
             "lat_p50_mean,lat_p50_stddev,"
             "lat_p99_mean,lat_p99_stddev\n";

        for (const auto& e : entries) {
            f << e.container_name << ","
              << e.scenario_name << ","
              << e.thread_count << ","
              << e.num_runs << ","
              << e.throughput_stats.mean << ","
              << e.throughput_stats.stddev << ","
              << e.throughput_stats.ci95_lo << ","
              << e.throughput_stats.ci95_hi << ","
              << e.throughput_stats.min_val << ","
              << e.throughput_stats.max_val << ","
              << e.lat_p50_stats.mean << ","
              << e.lat_p50_stats.stddev << ","
              << e.lat_p99_stats.mean << ","
              << e.lat_p99_stats.stddev << "\n";
        }
    }

    /// Записывает системные метрики отдельным файлом.
    void write_system_metrics(const std::string& container, const std::string& scenario,
                              uint32_t threads, uint32_t run,
                              const std::vector<SystemSample>& samples) {
        // При первом вызове — создаём заголовок
        static bool header_written = false;
        std::ios_base::openmode mode = header_written ? std::ios::app : std::ios::trunc;
        std::ofstream f(system_path_, mode);

        if (!header_written) {
            f << "container,scenario,threads,run,"
                 "timestamp_us,cpu_pct,rss_bytes,"
                 "vol_cs,invol_cs,page_faults,llc_misses\n";
            header_written = true;
        }

        for (const auto& s : samples) {
            f << container << "," << scenario << ","
              << threads << "," << run << ","
              << s.timestamp_us << ","
              << s.cpu_total_pct << ","
              << s.rss_bytes << ","
              << s.voluntary_cs << ","
              << s.involuntary_cs << ","
              << s.page_faults << ","
              << s.llc_misses << "\n";
        }
    }

    const std::string& raw_path() const { return raw_path_; }
    const std::string& summary_path() const { return summary_path_; }

private:
    std::string raw_path_;
    std::string summary_path_;
    std::string system_path_;
};

} // namespace bench
