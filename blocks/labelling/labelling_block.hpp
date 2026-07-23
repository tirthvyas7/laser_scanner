#pragma once

#include "block.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace line_scanner {

// One equivalence merge: absorbed id `from` folds into surviving id `to` (to < from).
struct MergeEvent {
    uint16_t from;
    uint16_t to;
};

// The Labelling->Tracing element (trivially-copyable POD). Value-initialised each
// cycle so unused slots are 0; id 0 doubles as the empty sentinel (0 = background).
struct LabelledElement {
    uint16_t   a, b;        // labels of the two pixels this cycle (0 = background)
    uint16_t   row, col;    // coords of `a`; `b` is at col+1
    MergeEvent merges[2];   // (from,to); empty slot = {0,0}  (<= 2 merges/cycle)
    uint16_t   retires[3];  // finished root ids; empty slot = 0 (<= 3 at row-end)
};

using LabelChannel = RingBuffer<LabelledElement>;

// Assignment budget: the two label-bookkeeping 1-D arrays are size m/2 each.
// Slots indexed by label id (0 = background => unused slot); usable ids 1..m/2-1.
// Shared so Tracing sizes its per-id table identically.
inline size_t labelArrayCap(size_t columns) { return columns / 2; }

// Third stage: streaming 8-connectivity connected-component labelling over the
// binary pair-stream. Single m-wide history row + two id-indexed arrays
// (parent_ = union-find + free-list overlay, count_ = per-id direct refcount).
// Emits one LabelledElement per input pair; recycles ids so memory is O(m).
class LabellingBlock : public Block {
   public:
    LabellingBlock();

    ChannelType outputType() const override { return ChannelType::LabelledElement; }
    void        setInputChannel(std::shared_ptr<ChannelBase> in) override;
    void        setOutputChannel(std::shared_ptr<ChannelBase> out) override;

    void configure(const PipelineConfig& cfg) override;
    void run() override;
    void stop() override;

   private:
    bool     readOnePair(PixelPair& out);
    void     processPair(uint8_t pl, uint8_t pr);
    void     writeElem(const LabelledElement& e);
    void     flushActiveOnClose();

    // Union-find + reclamation (all O(1) amortised; reclaim batched at row end).
    uint16_t find(uint16_t x);
    uint16_t unite(uint16_t a, uint16_t b);  // returns survivor (min), emits merge
    uint16_t classify(uint16_t nw, uint16_t n, uint16_t ne, uint16_t w);
    uint16_t relabelRoot(uint16_t& cell);    // resolve to root, move count onto it
    void     release(uint16_t id);           // a prev-row cell (recorded id) goes out of reach
    uint16_t newLabel();                      // pop free-list, else fresh
    void     reclaimRowEnd();                 // rebuild free-list from dead ids; measure peak

    std::shared_ptr<PixelChannel> in_;
    std::shared_ptr<LabelChannel> out_;
    std::atomic<bool>             stop_{false};

    // Working set (allocated in configure()).
    size_t                m_      = 0;  // columns
    size_t                idcap_  = 0;  // id pool size (ids 1..idcap_)
    std::vector<uint16_t> row_;         // [m_] history row, in place
    std::vector<uint16_t> parent_;      // [idcap_+1] union-find + free-list
    std::vector<uint16_t> count_;       // [idcap_+1] direct refcount per id

    // O(1) scalars.
    uint16_t free_head_ = 0;  // free-list head (0 = empty)
    uint16_t next_id_   = 1;  // next fresh id (id 0 reserved for background)
    uint16_t carry_     = 0;  // recorded root of prev-row col-1 (this cycle's NW)
    uint16_t cur_left_  = 0;  // label assigned at col-1 (this cycle's W)
    uint16_t row_idx_   = 0;
    uint16_t col_idx_   = 0;

    // Current cycle's output being built; ops write merges/retires here.
    LabelledElement cur_{};
    int             mn_ = 0;  // merge write index
    int             rn_ = 0;  // retire write index

    // Counters (surfaced as metrics later).
    uint64_t labels_completed_ = 0;
    uint64_t merges_performed_ = 0;
    uint32_t peak_active_      = 0;  // high-water of live ids (measured at row end)

    SourceMode mode_          = SourceMode::Rng;
    bool       terminal_      = false;
    uint64_t   cycle_time_ns_ = 1000;

    static constexpr size_t kPublishEvery = 100;
};

}  // namespace line_scanner
