#include "data_generation_block.hpp"

#include "block_factory.hpp"
#include "csv_source.hpp"
#include "rng_source.hpp"

#include <chrono>
#include <stdexcept>

namespace line_scanner {

DataGenerationBlock::DataGenerationBlock() : Block("DataGeneration") {}

void DataGenerationBlock::setInputChannel(std::shared_ptr<PixelChannel>) {
    // First stage — no input channel.
}

void DataGenerationBlock::setOutputChannel(std::shared_ptr<PixelChannel> out) {
    out_ = std::move(out);
}

void DataGenerationBlock::configure(const PipelineConfig& cfg) {
    cycle_time_ns_ = cfg.cycle_time_ns;
    if (cfg.mode == SourceMode::Csv) {
        source_ = std::make_unique<CsvSource>(cfg.csv_path, cfg.columns);
    } else {
        source_ = std::make_unique<RngSource>();
    }
}

void DataGenerationBlock::stop() {
    stop_.store(true, std::memory_order_release);
}

void DataGenerationBlock::busyWaitUntil(std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        LS_PAUSE();
    }
}

void DataGenerationBlock::run() {
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    if (!out_)
        throw std::runtime_error("DataGenerationBlock: output channel not set");
    if (!source_)
        throw std::runtime_error("DataGenerationBlock: source not configured");

    // Hot-path policy: the only work permitted between t_start and the busy-
    // wait is source.getNext(), out_->write(), one max compare, one violation
    // count, and one increment. NO divisions, NO running averages, NO writes
    // to seqlock-published memory. All averaging is deferred to the publish
    // step (every kPublishEvery cycles) and the final flush at exit. This is
    // what frees enough of T for the busy-wait to absorb minor jitter.

    BlockMetrics live{};

    // Per-cycle aggregates — local doubles, not seqlock-published.
    double cycle_sum_ns     = 0.0;
    double max_cycle_ns     = 0.0;
    size_t violations_local = 0;
    size_t cycles_local     = 0;
    size_t pixels_local     = 0;

    // Inter-pair output gap (the throughput metric) — track sum + max only.
    double gap_sum_ns       = 0.0;
    double max_gap_ns       = 0.0;
    size_t gap_count        = 0;
    auto   last_output_time = clock::time_point{};
    bool   have_last        = false;

    auto publishNow = [&]() {
        live.cycles_completed       = cycles_local;
        live.total_pixels_processed = pixels_local;
        live.avg_cycle_time_ns =
            cycles_local ? cycle_sum_ns / static_cast<double>(cycles_local) : 0.0;
        live.max_cycle_time_ns      = max_cycle_ns;
        live.constraint_violations  = violations_local;
        live.avg_throughput_ns      = gap_count ? gap_sum_ns / static_cast<double>(gap_count) : 0.0;
        live.max_throughput_ns      = max_gap_ns;
        live.peak_channel_occupancy = std::max(live.peak_channel_occupancy, out_->occupancy());
        publishMetrics(live);
    };

    while (!stop_.load(std::memory_order_acquire)) {
        const auto t_start  = clock::now();
        const auto deadline = t_start + ns(cycle_time_ns_);

        auto opt = source_->getNext();
        if (!opt.has_value()) {
            out_->close();
            stop_.store(true, std::memory_order_release);
            break;
        }

        while (!out_->write(*opt)) {
            if (stop_.load(std::memory_order_acquire)) {
                out_->close();
                publishNow();
                return;
            }
            LS_PAUSE();
        }

        const auto   t_after_write = clock::now();
        const double cycle_ns =
            static_cast<double>(std::chrono::duration_cast<ns>(t_after_write - t_start).count());

        // Hot-path metrics: counters + max compares only. No division.
        ++cycles_local;
        pixels_local += 2;
        cycle_sum_ns += cycle_ns;
        if (cycle_ns > max_cycle_ns)
            max_cycle_ns = cycle_ns;
        if (cycle_ns > static_cast<double>(cycle_time_ns_))
            ++violations_local;

        if (have_last) {
            const double dt = static_cast<double>(
                std::chrono::duration_cast<ns>(t_after_write - last_output_time).count());
            gap_sum_ns += dt;
            ++gap_count;
            if (dt > max_gap_ns)
                max_gap_ns = dt;
        }
        last_output_time = t_after_write;
        have_last        = true;

        if ((cycles_local % kPublishEvery) == 0)
            publishNow();

        // Rate-controlled: busy-wait the remainder of T. This is the slack
        // we just freed up by deferring the divisions.
        busyWaitUntil(deadline);
    }

    out_->close();
    publishNow();
}

}  // namespace line_scanner

LS_REGISTER_BLOCK(datagen, "DataGeneration", line_scanner::DataGenerationBlock)
