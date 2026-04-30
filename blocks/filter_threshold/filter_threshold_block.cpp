#include "filter_threshold_block.hpp"

#include "block_factory.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace line_scanner {

FilterThresholdBlock::FilterThresholdBlock() : Block("FilterThreshold") {}

void FilterThresholdBlock::setInputChannel(std::shared_ptr<PixelChannel> in) {
    in_ = std::move(in);
}

void FilterThresholdBlock::setOutputChannel(std::shared_ptr<PixelChannel> out) {
    out_ = std::move(out);
}

void FilterThresholdBlock::configure(const PipelineConfig& cfg) {
    threshold_     = cfg.threshold;
    cycle_time_ns_ = cfg.cycle_time_ns;
    output_buf_.clear();
    output_buf_.reserve(1 << 20);  // 1 MiB initial; grows as needed
    hist_.fill({});
}

void FilterThresholdBlock::stop() {
    stop_.store(true, std::memory_order_release);
}

bool FilterThresholdBlock::readOnePair(PixelPair& out) {
    while (!stop_.load(std::memory_order_acquire)) {
        if (in_->read(out))
            return true;
        if (in_->isClosed() && in_->isEmpty())
            return false;
        LS_PAUSE();
    }
    return false;
}

uint8_t FilterThresholdBlock::convolveAndThreshold(size_t center_idx) const {
    double sum = 0.0;
    for (int i = 0; i < static_cast<int>(kWindow); ++i) {
        const int    offset = i - 4;
        const size_t idx =
            (center_idx + static_cast<size_t>(offset + static_cast<int>(kWindow))) % kWindow;
        sum += static_cast<double>(window_[idx]) * kFilterWeights[i];
    }
    return (sum >= static_cast<double>(threshold_)) ? 1u : 0u;
}

void FilterThresholdBlock::run() {
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    if (!in_)
        throw std::runtime_error("FilterThresholdBlock: input channel not set");

    // Hot-path policy mirrors DataGen: counters + max compares only.
    // All divisions and seqlock writes happen at publish time (every
    // kPublishEvery cycles). Per-pixel/per-pair averaging is computed once
    // there from cycle_sum_ns / cycles_local, never on the per-cycle path.

    BlockMetrics live{};

    double cycle_sum_ns     = 0.0;
    double max_cycle_ns     = 0.0;
    size_t violations_local = 0;
    size_t cycles_local     = 0;
    size_t pixels_local     = 0;
    size_t peak_occ_local   = 0;

    size_t fill_count = 0;
    size_t head_pos   = 0;

    const auto       t_run_start           = clock::now();
    constexpr double kViolationThresholdNs = 100.0;

    auto publishNow = [&]() {
        live.cycles_completed       = cycles_local;
        live.total_pixels_processed = pixels_local;
        live.avg_cycle_time_ns =
            cycles_local ? cycle_sum_ns / static_cast<double>(cycles_local) : 0.0;
        live.max_cycle_time_ns      = max_cycle_ns;
        live.avg_throughput_ns      = live.avg_cycle_time_ns / 2.0;
        live.max_throughput_ns      = max_cycle_ns / 2.0;
        live.constraint_violations  = violations_local;
        live.peak_channel_occupancy = peak_occ_local;
        publishMetrics(live);
    };

    while (!stop_.load(std::memory_order_acquire)) {
        PixelPair pair;
        if (!readOnePair(pair))
            break;

        const auto t_process_start = clock::now();

        for (int side = 0; side < 2; ++side) {
            const uint8_t px = (side == 0) ? pair.first : pair.second;

            window_[head_pos] = px;
            head_pos          = (head_pos + 1) % kWindow;
            if (fill_count < kWindow)
                ++fill_count;

            if (fill_count == kWindow) {
                const size_t  center = (head_pos + 4) % kWindow;
                const uint8_t b      = convolveAndThreshold(center);
                output_buf_.push_back(b);  // in-memory; flushed at exit
                ++pixels_local;
            }
        }

        const auto t_process_end = clock::now();

        const double cycle_ns = static_cast<double>(
            std::chrono::duration_cast<ns>(t_process_end - t_process_start).count());

        ++cycles_local;
        cycle_sum_ns += cycle_ns;
        if (cycle_ns > max_cycle_ns)
            max_cycle_ns = cycle_ns;
        if (cycle_ns > static_cast<double>(cycle_time_ns_))
            ++violations_local;

        // Histogram bucket: cheap (one indexed array write + a few compares).
        const double elapsed_ms =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(t_process_end - t_run_start)
                    .count()) /
            1000.0;
        const size_t bucket_idx =
            std::min<size_t>(kBucketCount - 1, static_cast<size_t>(elapsed_ms / kBucketWidthMs));
        auto& b = hist_[bucket_idx];
        b.total_cycles += 1;
        const double per_pixel_ns = cycle_ns * 0.5;
        if (per_pixel_ns > kViolationThresholdNs) {
            b.violations += 1;
            b.sum_violation_cycle_ns += cycle_ns;
            if (cycle_ns > b.max_cycle_ns)
                b.max_cycle_ns = cycle_ns;
        }

        if ((cycles_local % kPublishEvery) == 0) {
            // Sample occupancy and the bucket's max-occupancy at publish time
            // to avoid the per-cycle atomic load + bucket update.
            const size_t occ = in_->occupancy();
            if (occ > peak_occ_local)
                peak_occ_local = occ;
            if (occ > b.max_occupancy)
                b.max_occupancy = occ;
            publishNow();
        }
    }

    if (out_)
        out_->close();
    publishNow();

    // Flush in-memory output buffer to disk (off the hot path).
    std::ofstream f(output_path_, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << "pixel_index,binary\n";
        for (size_t i = 0; i < output_buf_.size(); ++i) {
            f << i << ',' << static_cast<int>(output_buf_[i]) << '\n';
        }
    }

    // Dump violation histogram. Sparse — skip empty buckets. Each row is one
    // time window; columns let us spot accumulation (rate going up over
    // time), bursts (single bucket with huge max), or channel-pressure
    // correlation (max_occupancy at 8 = pipeline-bound).
    std::ofstream vf(violations_path_, std::ios::out | std::ios::trunc);
    if (vf.is_open()) {
        vf << "bucket_start_ms,bucket_end_ms,total_cycles,violations,"
              "violation_rate_pct,avg_violation_cycle_ns,max_cycle_ns,max_occupancy\n";
        for (size_t i = 0; i < hist_.size(); ++i) {
            const auto& b = hist_[i];
            if (b.total_cycles == 0)
                continue;
            const double rate_pct = b.total_cycles ? (100.0 * static_cast<double>(b.violations) /
                                                      static_cast<double>(b.total_cycles))
                                                   : 0.0;
            const double avg_v =
                b.violations ? b.sum_violation_cycle_ns / static_cast<double>(b.violations) : 0.0;
            vf << (i * kBucketWidthMs) << ',' << ((i + 1) * kBucketWidthMs) << ',' << b.total_cycles
               << ',' << b.violations << ',' << rate_pct << ',' << avg_v << ',' << b.max_cycle_ns
               << ',' << b.max_occupancy << '\n';
        }
    }
}

}  // namespace line_scanner

LS_REGISTER_BLOCK(filter, "FilterThreshold", line_scanner::FilterThresholdBlock)
