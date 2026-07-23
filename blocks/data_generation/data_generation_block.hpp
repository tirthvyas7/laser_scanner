#pragma once

#include "block.hpp"
#include "data_source.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace line_scanner {

// First stage of the pipeline. RATE-CONTROLLED: emulates a real line-scan
// camera by busy-waiting until exactly T nanoseconds have elapsed since the
// start of each cycle. Reads from a Strategy-pattern DataSource (CsvSource
// or RngSource) chosen at configure() time from PipelineConfig.mode.
class DataGenerationBlock : public Block {
   public:
    DataGenerationBlock();

    ChannelType outputType() const override { return ChannelType::PixelPair; }
    void        setInputChannel(std::shared_ptr<ChannelBase> in) override;
    void        setOutputChannel(std::shared_ptr<ChannelBase> out) override;

    void configure(const PipelineConfig& cfg) override;
    void run() override;
    void stop() override;

   private:
    void busyWaitUntil(std::chrono::steady_clock::time_point deadline);

    std::shared_ptr<PixelChannel> out_;
    std::unique_ptr<DataSource>   source_;
    uint64_t                      cycle_time_ns_ = 1000;
    std::atomic<bool>             stop_{false};

    static constexpr size_t kPublishEvery = 100;
};

}  // namespace line_scanner
