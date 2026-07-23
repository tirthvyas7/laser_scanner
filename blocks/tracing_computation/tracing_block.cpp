#include "tracing_block.hpp"

#include "block_factory.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace line_scanner {

TracingBlock::TracingBlock() : Block("Tracing") {}

void TracingBlock::setInputChannel(std::shared_ptr<ChannelBase> in) {
    in_ = std::dynamic_pointer_cast<LabelChannel>(in);
}

void TracingBlock::setOutputChannel(std::shared_ptr<ChannelBase> out) {
    out_ = std::dynamic_pointer_cast<DefectChannel>(out);
}

void TracingBlock::configure(const PipelineConfig& cfg) {
    cycle_time_ns_   = cfg.cycle_time_ns;
    output_path_     = cfg.output_path.empty() ? name_ + ".csv" : cfg.output_path;
    const size_t cap = labelArrayCap(cfg.columns);
    table_.assign(cap, LabelStats{});  // m/2 slots, id-indexed

    // One-time budget declaration: one m/2 per-label store (separate from Labelling's arrays).
    std::fprintf(stderr, "[Tracing] memory budget: table[m/2=%zu] x %zuB = %zu B\n",
                 cap, sizeof(LabelStats), cap * sizeof(LabelStats));
}

void TracingBlock::stop() {
    stop_.store(true, std::memory_order_release);
}

// --- per-label geometry -------------------------------------------------------

void TracingBlock::accumulate(uint16_t id, uint16_t row, uint16_t col) {
    LabelStats& s = table_[id];
    if (!s.active) {                                 // first pixel of this (recycled) id
        s = LabelStats{1, row, row, col, col, true};
        ++active_labels_;
        if (active_labels_ > peak_table_)
            peak_table_ = active_labels_;
        return;
    }
    ++s.size;
    if (row < s.top)    s.top    = row;
    if (row > s.bottom) s.bottom = row;
    if (col < s.left)   s.left   = col;
    if (col > s.right)  s.right  = col;
}

void TracingBlock::emitDefect(uint16_t id) {
    const LabelStats& s = table_[id];
    routeOut(DefectRecord{id, s.size, s.top, s.left, s.bottom, s.right});
    table_[id].active = false;                       // free the slot; recycled with the id
    --active_labels_;
    ++defects_emitted_;
}

void TracingBlock::routeOut(const DefectRecord& r) {
    if (terminal_) {
        sink_buf_.push_back(r);                      // buffer; flushed to CSV at exit
        return;
    }
    while (!out_->write(r)) {                         // rare (once/blob), off the hot path
        if (stop_.load(std::memory_order_acquire))
            return;
        LS_PAUSE();
    }
}

// --- run loop -----------------------------------------------------------------

bool TracingBlock::readOne(LabelledElement& out) {
    while (!stop_.load(std::memory_order_acquire)) {
        if (in_->read(out))
            return true;
        if (in_->isClosed() && in_->isEmpty())
            return false;
        LS_PAUSE();
    }
    return false;
}

void TracingBlock::run() {
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    if (!in_)
        throw std::runtime_error("TracingBlock: input channel not set");
    terminal_ = (out_ == nullptr);

    BlockMetrics live{};
    double       cycle_sum_ns     = 0.0;
    double       max_cycle_ns     = 0.0;
    size_t       violations_local = 0;
    size_t       cycles_local     = 0;
    size_t       pixels_local     = 0;
    size_t       peak_occ_local   = 0;

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
        LabelledElement e;
        if (!readOne(e))
            break;

        const auto t0 = clock::now();

        // Order matters: fold merges, then count pixels (post-find ids), then retire.
        for (const MergeEvent& mg : e.merges) {
            if (mg.from == 0)
                continue;                            // empty slot (0 = sentinel)
            LabelStats& A = table_[mg.from];
            LabelStats& B = table_[mg.to];
            if (A.active) {
                if (!B.active) {                     // survivor seeded from the victim
                    B = A;
                } else {
                    B.size += A.size;
                    if (A.top    < B.top)    B.top    = A.top;
                    if (A.bottom > B.bottom) B.bottom = A.bottom;
                    if (A.left   < B.left)   B.left   = A.left;
                    if (A.right  > B.right)  B.right  = A.right;
                }
                A.active = false;                    // 'from' folded away
                --active_labels_;
            }
        }

        if (e.a)
            accumulate(e.a, e.row, e.col);
        if (e.b)
            accumulate(e.b, e.row, e.col + 1);

        for (uint16_t id : e.retires)
            if (id)
                emitDefect(id);

        const auto   t1 = clock::now();
        const double cycle_ns =
            static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count());
        ++cycles_local;
        pixels_local += (e.a ? 1 : 0) + (e.b ? 1 : 0);
        cycle_sum_ns += cycle_ns;
        if (cycle_ns > max_cycle_ns)
            max_cycle_ns = cycle_ns;
        if (cycle_ns > static_cast<double>(cycle_time_ns_))
            ++violations_local;

        if ((cycles_local % kPublishEvery) == 0) {
            const size_t occ = in_->occupancy();
            if (occ > peak_occ_local)
                peak_occ_local = occ;
            publishNow();
        }
    }

    if (out_)
        out_->close();
    publishNow();

    // Sink only: dump finished defects to CSV (off hot path).
    if (terminal_) {
        std::ofstream f(output_path_, std::ios::out | std::ios::trunc);
        if (f.is_open()) {
            std::string buf = DefectRecord::csvHeader();
            buf += '\n';
            for (const DefectRecord& r : sink_buf_)
                r.csvRow(buf);
            f << buf;
        }
    }
}

}  // namespace line_scanner

LS_REGISTER_BLOCK(tracing, "Tracing", line_scanner::TracingBlock)
