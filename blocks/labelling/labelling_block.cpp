#include "labelling_block.hpp"

#include "block_factory.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace line_scanner {

LabellingBlock::LabellingBlock() : Block("Labelling") {}

void LabellingBlock::setInputChannel(std::shared_ptr<ChannelBase> in) {
    in_ = std::dynamic_pointer_cast<PixelChannel>(in);
}

void LabellingBlock::setOutputChannel(std::shared_ptr<ChannelBase> out) {
    out_ = std::dynamic_pointer_cast<LabelChannel>(out);
}

void LabellingBlock::configure(const PipelineConfig& cfg) {
    m_             = cfg.columns;
    mode_          = cfg.mode;
    cycle_time_ns_ = cfg.cycle_time_ns;
    idcap_ = labelArrayCap(m_);  // strict m/2 (assignment budget); usable ids 1..idcap_-1
    row_.assign(m_, 0);
    parent_.assign(idcap_, 0);
    count_.assign(idcap_, 0);

    // One-time budget declaration: exactly one m-row + two m/2 arrays (uint16).
    std::fprintf(stderr, "[Labelling] memory budget: row[m=%zu] + parent[m/2=%zu] + count[m/2=%zu] uint16 = %zu B\n",
                 m_, idcap_, idcap_, (m_ + 2 * idcap_) * sizeof(uint16_t));
}

void LabellingBlock::stop() {
    stop_.store(true, std::memory_order_release);
}

// --- union-find + reclamation -------------------------------------------------

uint16_t LabellingBlock::find(uint16_t x) {
    if (x == 0)
        return 0;
    uint16_t r = x;
    while (parent_[r] != r)
        r = parent_[r];
    while (parent_[x] != r) {  // path compression
        uint16_t nx = parent_[x];
        parent_[x]  = r;
        x           = nx;
    }
    return r;
}

uint16_t LabellingBlock::unite(uint16_t a, uint16_t b) {
    uint16_t ra = find(a), rb = find(b);
    if (ra == rb)
        return ra;
    uint16_t lo = ra < rb ? ra : rb;  // lower id survives
    uint16_t hi = ra < rb ? rb : ra;
    parent_[hi] = lo;                 // one flip; counts NOT folded (drain via relabel)
    if (mn_ < 2) {
        cur_.merges[mn_].from = hi;
        cur_.merges[mn_].to   = lo;
        ++mn_;
    }
    ++merges_performed_;
    return lo;
}

uint16_t LabellingBlock::classify(uint16_t nw, uint16_t n, uint16_t ne, uint16_t w) {
    if (n)   n  = find(n);            // resolve (neighbours may be stale)
    if (ne)  ne = find(ne);
    if (nw)  nw = find(nw);
    if (w)   w  = find(w);
    if (n)   return n;               // copy — no merge
    if (ne) {
        if (nw)  return unite(ne, nw);  // valley join
        if (w)   return unite(ne, w);
        return ne;
    }
    if (nw)  return nw;
    if (w)   return w;
    return newLabel();
}

uint16_t LabellingBlock::relabelRoot(uint16_t& cell) {
    if (cell == 0)
        return 0;
    uint16_t r = find(cell);
    if (cell != r) {                 // move this cell's count onto its root
        --count_[cell];
        ++count_[r];
        cell = r;
    }
    return r;
}

void LabellingBlock::release(uint16_t id) {
    if (id == 0)
        return;
    if (--count_[id] == 0 && parent_[id] == id) {  // a ROOT with no cells left = finished
        if (rn_ < 3)
            cur_.retires[rn_++] = id;
        ++labels_completed_;
    }
    // No free here: row-end reclaim recovers the slot once all refs are compressed.
}

uint16_t LabellingBlock::newLabel() {
    uint16_t id;
    if (free_head_) {                          // reuse a slot freed at a prior row end
        id         = free_head_;
        free_head_ = parent_[id];
    } else {
        if (next_id_ >= idcap_)  // ids valid 1..idcap_-1
            throw std::runtime_error("Labelling: id pool exhausted (m/2 arrays)");
        id = next_id_++;
    }
    parent_[id] = id;                          // makeSet; caller does count_[id]++
    count_[id]  = 0;
    return id;
}

void LabellingBlock::reclaimRowEnd() {
    // Every cell has been read+compressed exactly once this row, so any id with
    // count 0 is provably unreferenced -> free ⇔ count==0. Rebuild the free-list.
    free_head_     = 0;
    uint32_t live  = 0;
    for (uint16_t id = 1; id < next_id_; ++id) {
        if (count_[id] == 0) {
            parent_[id] = free_head_;  // thread free-list link
            free_head_  = id;
        } else {
            ++live;
        }
    }
    if (live > peak_active_)
        peak_active_ = live;
}

// --- per-pair processing ------------------------------------------------------

void LabellingBlock::processPair(uint8_t pl, uint8_t pr) {
    const size_t c = col_idx_;
    cur_.row       = row_idx_;
    cur_.col       = static_cast<uint16_t>(c);

    const uint16_t rec_cm1 = carry_;                 // recorded root of prev-row c-1
    uint16_t       N_L     = relabelRoot(row_[c]);   // prev-row c   -> root (count moved)
    uint16_t       NE_L    = relabelRoot(row_[c + 1]);  // prev-row c+1 -> root

    // Left pixel (col c). Neighbours: NW=rec_cm1, N=N_L, NE=NE_L, W=cur_left_.
    uint16_t Lc = pl ? classify(rec_cm1, N_L, NE_L, cur_left_) : 0;

    // Right pixel (col c+1). NW=prev c (N_L), N=prev c+1 (NE_L), NE=prev c+2, W=Lc.
    uint16_t NE_R = (c + 2 < m_) ? relabelRoot(row_[c + 2]) : 0;
    uint16_t Lc1  = pr ? classify(N_L, NE_L, NE_R, Lc) : 0;

    // Commit: increments FIRST, then the two lagged releases, then roll carry.
    if (Lc)
        ++count_[Lc];
    if (Lc1)
        ++count_[Lc1];
    release(rec_cm1);   // prev-row c-1 out of reach
    release(N_L);       // prev-row c   out of reach
    carry_    = NE_L;   // prev-row c+1 -> next cycle's NW
    row_[c]     = Lc;
    row_[c + 1] = Lc1;
    cur_left_   = Lc1;

    cur_.a = Lc;
    cur_.b = Lc1;
}

// --- run loop -----------------------------------------------------------------

bool LabellingBlock::readOnePair(PixelPair& out) {
    while (!stop_.load(std::memory_order_acquire)) {
        if (in_->read(out))
            return true;
        if (in_->isClosed() && in_->isEmpty())
            return false;
        LS_PAUSE();
    }
    return false;
}

void LabellingBlock::writeElem(const LabelledElement& e) {
    while (!out_->write(e)) {
        if (stop_.load(std::memory_order_acquire))
            return;
        LS_PAUSE();
    }
}

void LabellingBlock::flushActiveOnClose() {
    // Last row's roots never get a next-row release: retire every live root.
    LabelledElement e{};
    int             rn = 0;
    for (uint16_t id = 1; id < next_id_; ++id) {
        if (count_[id] > 0 && parent_[id] == id) {
            e.retires[rn++] = id;
            ++labels_completed_;
            if (rn == 3) {
                if (!terminal_)
                    writeElem(e);
                e  = LabelledElement{};
                rn = 0;
            }
        }
    }
    if (rn > 0 && !terminal_)
        writeElem(e);
}

void LabellingBlock::run() {
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    if (!in_)
        throw std::runtime_error("LabellingBlock: input channel not set");
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
        PixelPair pair;
        if (!readOnePair(pair))
            break;

        cur_ = LabelledElement{};  // value-init: unused slots 0
        mn_  = 0;
        rn_  = 0;

        const auto t0 = clock::now();

        processPair(pair.first, pair.second);

        // Advance scan position; at row end retire the carried tail + reclaim.
        col_idx_ += 2;
        if (col_idx_ >= m_) {
            release(carry_);  // prev-row m-1 (rides in the same element, retires[<=3])
            carry_    = 0;
            cur_left_ = 0;
            ++row_idx_;
            col_idx_ = 0;
            reclaimRowEnd();
        }

        const auto   t1 = clock::now();
        const double cycle_ns =
            static_cast<double>(std::chrono::duration_cast<ns>(t1 - t0).count());
        ++cycles_local;
        pixels_local += 2;
        cycle_sum_ns += cycle_ns;
        if (cycle_ns > max_cycle_ns)
            max_cycle_ns = cycle_ns;
        if (cycle_ns > static_cast<double>(cycle_time_ns_))
            ++violations_local;

        if (!terminal_)
            writeElem(cur_);

        if ((cycles_local % kPublishEvery) == 0) {
            const size_t occ = in_->occupancy();
            if (occ > peak_occ_local)
                peak_occ_local = occ;
            publishNow();
        }
    }

    flushActiveOnClose();
    if (out_)
        out_->close();
    publishNow();
}

}  // namespace line_scanner

LS_REGISTER_BLOCK(labelling, "Labelling", line_scanner::LabellingBlock)
