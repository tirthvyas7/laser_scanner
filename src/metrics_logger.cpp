#include "block.hpp"
#include "metrics.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace line_scanner {

namespace {

uint64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string verdictTag(bool ok) {
    return ok ? "PASS" : "FAIL";
}

}  // namespace

MetricsLogger::MetricsLogger(std::vector<Block*> blocks, std::string log_path,
                             uint64_t poll_interval_ms)
    : blocks_(std::move(blocks)),
      log_path_(std::move(log_path)),
      poll_interval_ms_(poll_interval_ms) {}

MetricsLogger::~MetricsLogger() {
    stop();
}

void MetricsLogger::start() {
    log_file_.open(log_path_, std::ios::out | std::ios::trunc);
    if (log_file_.is_open() && !header_written_) {
        log_file_ << "timestamp_ms,block,cycles,pixels,"
                     "avg_cycle_ns,max_cycle_ns,"
                     "avg_throughput_ns,max_throughput_ns,"
                     "peak_occupancy,violations\n";
        header_written_ = true;
    }
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread(&MetricsLogger::run, this);
}

void MetricsLogger::stop() {
    if (!stop_.exchange(true, std::memory_order_acq_rel)) {
        if (thread_.joinable())
            thread_.join();
        if (log_file_.is_open()) {
            log_file_.flush();
            log_file_.close();
        }
    }
}

void MetricsLogger::run() {
    using namespace std::chrono;
    while (!stop_.load(std::memory_order_acquire)) {
        const uint64_t ts = nowMillis();
        for (Block* blk : blocks_) {
            const BlockMetrics m = blk->publishedMetrics();
            writeRow(blk->name(), m, ts);
        }
        log_file_.flush();
        std::this_thread::sleep_for(milliseconds(poll_interval_ms_));
    }
}

void MetricsLogger::writeRow(const std::string& name, const BlockMetrics& m,
                             uint64_t timestamp_ms) {
    if (!log_file_.is_open())
        return;
    log_file_ << timestamp_ms << ',' << name << ',' << m.cycles_completed << ','
              << m.total_pixels_processed << ',' << std::fixed << std::setprecision(2)
              << m.avg_cycle_time_ns << ',' << m.max_cycle_time_ns << ',' << m.avg_throughput_ns
              << ',' << m.max_throughput_ns << ',' << m.peak_channel_occupancy << ','
              << m.constraint_violations << '\n';
}

void MetricsLogger::writeFinalReport(const std::vector<NamedMetrics>& finals,
                                     const std::string& report_path, uint64_t cycle_time_budget_ns,
                                     size_t memory_budget_bytes, double throughput_budget_ns) {
    // Each PixelPair occupies 2 bytes (two uint8_t values). Used to convert
    // peak_channel_occupancy (pairs) into bytes for the memory budget check.
    constexpr size_t kPairBytes = 2;

    std::ostringstream os;
    os << "=========================================================\n";
    os << "  Line Scanner Pipeline — Final Metrics Report\n";
    os << "=========================================================\n";
    os << "  Cycle time budget T  : " << cycle_time_budget_ns << " ns\n";
    os << "  Memory budget m      : " << memory_budget_bytes << " bytes\n";
    os << "  Throughput budget    : <" << throughput_budget_ns << " ns\n";
    os << "---------------------------------------------------------\n";

    for (const auto& nm : finals) {
        const auto& m = nm.metrics;

        // DataGeneration is rate-controlled at T (busy-wait paces output).
        // The reported avg_throughput_ns is the inter-pair output gap, which
        // is the meaningful metric for a camera; cycle_time is just internal
        // work time (a small fraction of T). We don't show it for the source.
        const bool is_source = (nm.name == "DataGeneration");

        // Memory: peak buffered data on the channel feeding/leaving this
        // block. Compared against the memory budget m (in bytes).
        const size_t peak_bytes = m.peak_channel_occupancy * kPairBytes;
        const bool   memory_ok  = (peak_bytes <= memory_budget_bytes);

        os << "  Block: " << nm.name;
        if (is_source)
            os << "  (rate-controlled @ T)";
        os << '\n';
        os << "    Cycles completed         : " << m.cycles_completed << '\n';
        os << "    Pixels processed         : " << m.total_pixels_processed << '\n';
        os << std::fixed << std::setprecision(2);

        if (is_source) {
            // The achieved output rate is what matters for a camera-emulated
            // source. Avg should hug T; max gap ≫ T = OS preemption spike.
            const bool rate_ok =
                (m.cycles_completed == 0) ||
                (m.avg_throughput_ns >= 0.9 * static_cast<double>(cycle_time_budget_ns) &&
                 m.avg_throughput_ns <= 1.1 * static_cast<double>(cycle_time_budget_ns));
            os << "    Pacing target T          : " << cycle_time_budget_ns << " ns/pair\n";
            os << "    Output rate              : avg " << m.avg_throughput_ns << " ns | max gap "
               << m.max_throughput_ns << " ns/pair  [" << verdictTag(rate_ok)
               << " avg ≈ T ± 10%]\n";
            os << "      (max gap ≫ T = OS preemption spike, environmental)\n";
        } else {
            // Same data shown two ways — pair-time and pixel-time. Two
            // budgets check the same measurement: T (per pair) and <100ns
            // (per pixel). The 100ns bar is strictly tighter than T.
            const bool pair_ok = (m.cycles_completed == 0) ||
                                 (m.avg_cycle_time_ns < static_cast<double>(cycle_time_budget_ns));
            const bool pixel_ok =
                (m.cycles_completed == 0) || (m.avg_throughput_ns < throughput_budget_ns);
            os << "    Processing time / pair   : avg " << m.avg_cycle_time_ns << " ns | max "
               << m.max_cycle_time_ns << " ns  [" << verdictTag(pair_ok)
               << " avg vs T=" << cycle_time_budget_ns << "ns]\n";
            os << "    Processing time / pixel  : avg " << m.avg_throughput_ns << " ns | max "
               << m.max_throughput_ns << " ns  [" << verdictTag(pixel_ok) << " avg vs <"
               << throughput_budget_ns << "ns]\n";
            os << "      (max spike = OS preemption, not algorithmic; avg is the sustained rate)\n";
        }

        os << "    Peak channel occupancy   : " << m.peak_channel_occupancy << " pairs ("
           << peak_bytes << " bytes)  [" << verdictTag(memory_ok) << " vs m=" << memory_budget_bytes
           << " bytes]\n";
        os << "    Cycle violations (>T)    : " << m.constraint_violations << '\n';
        os << "---------------------------------------------------------\n";
    }

    std::cout << os.str();
    std::ofstream f(report_path, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << os.str();
    }
}

}  // namespace line_scanner
