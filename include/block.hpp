#pragma once

#include "metrics.hpp"
#include "pipeline_config.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace line_scanner {

using PixelChannel = RingBuffer<PixelPair>;

// Abstract base for every pipeline stage. Carries seqlock infrastructure
// so the LoggerThread can read each block's metrics without ever blocking
// the block's own run() loop.
//
// Seqlock contract:
//   Writer (block thread, inside run()) calls publishMetrics(live):
//     1. version_ becomes odd  -> "writing in progress"
//     2. plain copy live -> published_
//     3. version_ becomes even -> "consistent again"
//   Block thread NEVER waits.
//
//   Reader (logger thread or final-report path) calls publishedMetrics():
//     1. read v1 = version_
//     2. if v1 is odd, retry
//     3. copy published_ to local
//     4. read v2 = version_
//     5. if v1 != v2, retry
//   Logger thread may retry, but never holds anything the writer needs.
class Block {
   public:
    virtual ~Block() = default;

    virtual void setInputChannel(std::shared_ptr<PixelChannel> in)   = 0;
    virtual void setOutputChannel(std::shared_ptr<PixelChannel> out) = 0;

    virtual void configure(const PipelineConfig& cfg) = 0;
    virtual void run()                                = 0;
    virtual void stop()                               = 0;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] BlockMetrics publishedMetrics() const noexcept;

   protected:
    explicit Block(std::string name) : name_(std::move(name)) {}

    void publishMetrics(const BlockMetrics& live) noexcept;

    std::string name_;

   private:
    mutable BlockMetrics published_{};
    alignas(64) std::atomic<uint64_t> version_{0};
};

}  // namespace line_scanner
