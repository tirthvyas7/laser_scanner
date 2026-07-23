#include "filter_threshold_block.hpp"

#include "block_factory.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace line_scanner {

FilterThresholdBlock::FilterThresholdBlock() : Block("FilterThreshold") {}

void FilterThresholdBlock::setInputChannel(std::shared_ptr<ChannelBase> in) {
    in_ = std::dynamic_pointer_cast<PixelChannel>(in);
}

void FilterThresholdBlock::setOutputChannel(std::shared_ptr<ChannelBase> out) {
    out_ = std::dynamic_pointer_cast<PixelChannel>(out);
}

void FilterThresholdBlock::configure(const PipelineConfig& cfg) {
    threshold_     = cfg.threshold;
    cycle_time_ns_ = cfg.cycle_time_ns;
    // out_ isn't wired until after configure(); output_buf_ setup deferred to run().
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

    // No output channel => sink (writes filter_output.csv); otherwise forward downstream.
    const bool terminal = (out_ == nullptr);
    if (terminal) {
        output_buf_.clear();
        output_buf_.reserve(1 << 20);  // 1 MiB initial; grows as needed
    }

    // Hot-path: counters + max compares only; averaging deferred to publishNow().
    BlockMetrics live{};

    double cycle_sum_ns     = 0.0;
    double max_cycle_ns     = 0.0;
    size_t violations_local = 0;
    size_t cycles_local     = 0;
    size_t pixels_local     = 0;
    size_t peak_occ_local   = 0;

    size_t fill_count = 0;
    size_t head_pos   = 0;

    // 9-tap warmup (8 px = 4 pairs) => output is always 2/cycle = one whole PixelPair.
    bool downstream_stop = false;

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
        live.channel_capacity       = in_->capacity();
        live.channel_elem_bytes     = in_->memoryBytes() / in_->capacity();
        publishMetrics(live);
    };

    while (!stop_.load(std::memory_order_acquire)) {
        PixelPair pair;
        if (!readOnePair(pair))
            break;

        uint8_t cyc_out[2];  // this cycle's outputs (<=2, one per pixel)
        size_t  cyc_n = 0;

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
                cyc_out[cyc_n++]     = b;      // stage for forwarding
                if (terminal)
                    output_buf_.push_back(b);  // sink keeps it for the file
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

        // Forward downstream, off the timed path: back-pressure is not algorithmic time.
        if (!terminal && cyc_n == 2) {
            const PixelPair pp{cyc_out[0], cyc_out[1]};
            while (!out_->write(pp)) {
                if (stop_.load(std::memory_order_acquire)) {
                    downstream_stop = true;
                    break;
                }
                LS_PAUSE();
            }
            if (downstream_stop)
                break;
        }

        if ((cycles_local % kPublishEvery) == 0) {
            const size_t occ = in_->occupancy();  // sample at publish, not per-cycle
            if (occ > peak_occ_local)
                peak_occ_local = occ;
            publishNow();
        }
    }

    if (out_)
        out_->close();
    publishNow();

    // Sink only: flush processed stream to disk (off hot path).
    if (terminal) {
        std::ofstream f(output_path_, std::ios::out | std::ios::trunc);
        if (f.is_open()) {
            f << "pixel_index,binary\n";
            for (size_t i = 0; i < output_buf_.size(); ++i) {
                f << i << ',' << static_cast<int>(output_buf_[i]) << '\n';
            }
        }
    }
}

}  // namespace line_scanner

LS_REGISTER_BLOCK(filter, "FilterThreshold", line_scanner::FilterThresholdBlock)
