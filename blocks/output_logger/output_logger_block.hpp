#pragma once

#include "block.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <string>

namespace line_scanner {

// Generic terminal sink: dumps whatever record type T arrives on its input
// channel to "<name>.csv". Type-blind except for serialization, which T
// supplies itself via two traits (compile-time, so the wire stays POD):
//   static const char* T::csvHeader();
//   void T::csvRow(std::string& out) const;
// Pure sink: no output channel, and it publishes NO metrics (nothing
// meaningful to time — it just serializes finished records off the hot path).
template <class T>
class OutputLoggerBlock : public Block {
   public:
    explicit OutputLoggerBlock(std::string name) : Block(std::move(name)) {}

    ChannelType outputType() const override { return ChannelType::None; }

    void setInputChannel(std::shared_ptr<ChannelBase> in) override {
        in_ = std::dynamic_pointer_cast<RingBuffer<T>>(in);
    }
    void setOutputChannel(std::shared_ptr<ChannelBase>) override {}  // terminal: no output

    void configure(const PipelineConfig& cfg) override {
        output_path_ = cfg.output_path.empty() ? name_ + ".csv" : cfg.output_path;
    }

    void stop() override { stop_.store(true, std::memory_order_release); }

    void run() override {
        if (!in_)
            throw std::runtime_error("OutputLoggerBlock: input channel not set");

        std::string buf = T::csvHeader();
        buf += '\n';

        T rec;
        while (!stop_.load(std::memory_order_acquire)) {
            if (in_->read(rec)) {
                rec.csvRow(buf);                             // T formats itself; block stays type-blind
            } else if (in_->isClosed() && in_->isEmpty()) {
                break;
            } else {
                LS_PAUSE();
            }
        }

        // Single exit-time flush (records are rare; off the hot path).
        std::ofstream f(output_path_, std::ios::out | std::ios::trunc);
        if (f.is_open())
            f << buf;
    }

   private:
    std::shared_ptr<RingBuffer<T>> in_;
    std::atomic<bool>              stop_{false};
    std::string                    output_path_;  // from config, else <name>.csv (configure())
};

}  // namespace line_scanner
