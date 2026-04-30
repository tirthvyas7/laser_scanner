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
// Window initialization: the first 4 outputs use zero-padded history,
// so they are emitted only after enough pixels have arrived to fill
// the future half of the window (4 lookahead pixels).
class FilterThresholdBlock : public Block {
   public:
    FilterThresholdBlock();

    void setInputChannel(std::shared_ptr<PixelChannel> in) override;
    void setOutputChannel(std::shared_ptr<PixelChannel> out) override;

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

    // In-memory output buffer; flushed to filter_output.csv at run() exit.
    // Keeping the per-pixel formatted ofstream OUT of the hot path was the
    // single biggest throughput fix — std::ofstream<<int costs ~100-200ns.
    std::vector<uint8_t> output_buf_;
    std::string          output_path_ = "filter_output.csv";

    // Violation histogram: bucketed by elapsed time so we get full coverage
    // of the run, not just the first N. Each bucket aggregates total cycles
    // and violation count + max cycle_ns + max occupancy. Dumped to
    // filter_violations.csv at exit. Lets us see if violation RATE drifts
    // over time (the "accumulation" hypothesis) rather than just sampling
    // the noisy startup window.
    struct ViolationBucket {
        uint64_t total_cycles           = 0;
        uint64_t violations             = 0;
        double   max_cycle_ns           = 0.0;
        double   sum_violation_cycle_ns = 0.0;
        size_t   max_occupancy          = 0;
    };
    static constexpr size_t                   kBucketWidthMs = 100;
    static constexpr size_t                   kBucketCount   = 600;  // 60s coverage
    std::array<ViolationBucket, kBucketCount> hist_{};
    std::string                               violations_path_ = "filter_violations.csv";

    // 9-tap window. Indices wrap modulo 9 via window_pos_.
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
