#pragma once

#include "block.hpp"
#include "labelling_block.hpp"  // LabelledElement (input contract)

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace line_scanner {

// One finished defect: its label, pixel count, and smallest enclosing rectangle.
// Trivially-copyable POD; carries its own CSV trait so OutputLogger stays generic.
struct DefectRecord {
    uint16_t label;
    uint32_t size;             // total pixels in the blob
    uint16_t top, left;        // top-left corner of the bounding box
    uint16_t bottom, right;    // bottom-right corner

    static const char* csvHeader() { return "label,size,top,left,bottom,right"; }
    void               csvRow(std::string& out) const {
        out += std::to_string(label) + ',' + std::to_string(size) + ',' + std::to_string(top) +
               ',' + std::to_string(left) + ',' + std::to_string(bottom) + ',' +
               std::to_string(right) + '\n';
    }
};

using DefectChannel = RingBuffer<DefectRecord>;

// Fourth stage: per-label running size + bounding box. Folds merges, accumulates
// each pixel's coords, and on retire emits a DefectRecord (size + rectangle) then
// frees the slot. O(1) per element; table_ indexed by the recycled label id.
class TracingBlock : public Block {
   public:
    TracingBlock();

    ChannelType outputType() const override { return ChannelType::DefectRecord; }
    void        setInputChannel(std::shared_ptr<ChannelBase> in) override;
    void        setOutputChannel(std::shared_ptr<ChannelBase> out) override;

    void configure(const PipelineConfig& cfg) override;
    void run() override;
    void stop() override;

   private:
    struct LabelStats {
        uint32_t size;
        uint16_t top, bottom, left, right;
        bool     active;
    };

    bool readOne(LabelledElement& out);
    void accumulate(uint16_t id, uint16_t row, uint16_t col);
    void emitDefect(uint16_t id);      // build+route a DefectRecord, free the slot
    void routeOut(const DefectRecord& r);

    std::shared_ptr<LabelChannel>  in_;
    std::shared_ptr<DefectChannel> out_;
    std::atomic<bool>              stop_{false};

    std::vector<LabelStats> table_;   // [idcap_+1] indexed by label id

    // Terminal-mode sink buffer (flushed to CSV at exit, off the hot path).
    std::vector<DefectRecord> sink_buf_;

    // Counters (surfaced as metrics later).
    uint64_t defects_emitted_ = 0;
    uint32_t active_labels_   = 0;
    uint32_t peak_table_      = 0;

    bool     terminal_      = false;
    uint64_t cycle_time_ns_ = 1000;
    std::string output_path_;  // from config, else <name>.csv (set in configure())

    static constexpr size_t kPublishEvery = 100;
};

}  // namespace line_scanner
