#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace line_scanner {

// Plain-old-data so it can be copied byte-for-byte under the seqlock.
// The block's name is stored separately on the Block object (set once at
// construction, never updated) — keeping it out of this struct keeps the
// snapshot trivially copyable, which the seqlock relies on.
struct BlockMetrics {
    double avg_cycle_time_ns      = 0.0;
    double max_cycle_time_ns      = 0.0;
    double avg_throughput_ns      = 0.0;
    double max_throughput_ns      = 0.0;
    size_t peak_channel_occupancy = 0;
    size_t total_pixels_processed = 0;
    size_t constraint_violations  = 0;
    size_t cycles_completed       = 0;
    size_t channel_capacity       = 0;  // measured channel capacity (elements)
    size_t channel_elem_bytes     = 0;  // real byte size of one channel element
};

struct NamedMetrics {
    std::string  name;
    BlockMetrics metrics;
};

class Block;

class MetricsLogger {
   public:
    MetricsLogger(std::vector<Block*> blocks, std::string log_path, uint64_t poll_interval_ms = 50);
    ~MetricsLogger();

    MetricsLogger(const MetricsLogger&)            = delete;
    MetricsLogger& operator=(const MetricsLogger&) = delete;

    void start();
    void stop();

    static void writeFinalReport(const std::vector<NamedMetrics>& finals,
                                 const std::string& report_path, uint64_t cycle_time_budget_ns,
                                 size_t memory_budget_bytes, double throughput_budget_ns = 100.0);

   private:
    void run();
    void writeRow(const std::string& name, const BlockMetrics& m, uint64_t timestamp_ms);

    std::vector<Block*> blocks_;
    std::string         log_path_;
    uint64_t            poll_interval_ms_;
    std::atomic<bool>   stop_{false};
    std::thread         thread_;
    std::ofstream       log_file_;
    bool                header_written_{false};
};

}  // namespace line_scanner
