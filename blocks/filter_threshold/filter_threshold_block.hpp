#pragma once

#include "block.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace line_scanner {

// Second stage of the pipeline. RATE-FOLLOWING: runs as fast as data
// arrives. Maintains a 9-tap sliding window over the incoming pixel
// stream, applies a fixed Gaussian-like convolution kernel, and
// thresholds the result against TV to produce a binary output.
//
// Output routing: if an output channel is wired (i.e. another block follows
// in the pipeline), the binary output is forwarded downstream, packed two
// bytes per PixelPair. If no output channel is set, this block is the sink
// and instead writes the stream to filter_output.csv.
//
// Window initialization: the first 4 outputs use zero-padded history,
// so they are emitted only after enough pixels have arrived to fill
// the future half of the window (4 lookahead pixels).
class FilterThresholdBlock : public Block {
   public:
    FilterThresholdBlock();

    ChannelType outputType() const override { return ChannelType::PixelPair; }
    void        setInputChannel(std::shared_ptr<ChannelBase> in) override;
    void        setOutputChannel(std::shared_ptr<ChannelBase> out) override;

    void configure(const PipelineConfig& cfg) override;
    void run() override;
    void stop() override;

   private:
    bool    readOnePair(PixelPair& out);
    uint8_t convolveAndThreshold(size_t center_idx) const;

    std::shared_ptr<PixelChannel> in_;
    std::shared_ptr<PixelChannel> out_;
    uint8_t                       threshold_     = 128;
    uint64_t                      cycle_time_ns_ = 1000;
    std::atomic<bool>             stop_{false};

    // In-memory output buffer, used only when this block is the sink;
    // flushed to filter_output.csv at run() exit. Keeping the per-pixel
    // formatted ofstream OUT of the hot path was the single biggest
    // throughput fix — std::ofstream<<int costs ~100-200ns.
    std::vector<uint8_t> output_buf_;
    std::string          output_path_ = "filter_output.csv";

    // 9-tap window. Indices wrap modulo 9 via head_pos.
    static constexpr size_t      kWindow = 9;
    std::array<uint8_t, kWindow> window_{};

    static constexpr size_t kPublishEvery = 100;
};

// Convolution weights — verbatim from the assignment specification.
// NOTE: K+4 (0.000125885) is asymmetric versus K-4 (0.000251770);
// suspected typo in the spec but used as given.
inline constexpr std::array<double, 9> kFilterWeights = {
    0.000251770,  // K-4
    0.008666992,  // K-3
    0.078025818,  // K-2
    0.241302490,  // K-1
    0.343757629,  // K
    0.241302490,  // K+1
    0.078025818,  // K+2
    0.008666992,  // K+3
    0.000125885   // K+4
};

}  // namespace line_scanner
