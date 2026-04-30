#include "block.hpp"

#include "ring_buffer.hpp"

namespace line_scanner {

void Block::publishMetrics(const BlockMetrics& live) noexcept {
    // Mark "writing in progress" — version becomes odd.
    version_.fetch_add(1, std::memory_order_release);

    // Plain copy. Looks unsafe, but the seqlock makes a torn read detectable
    // by readers (they see mismatching version stamps and retry).
    published_ = live;

    // Mark "consistent again" — version becomes even and matches the v1 a
    // reader saw before the data write.
    version_.fetch_add(1, std::memory_order_release);
}

BlockMetrics Block::publishedMetrics() const noexcept {
    BlockMetrics out;
    uint64_t     v1 = 0, v2 = 0;
    do {
        v1 = version_.load(std::memory_order_acquire);
        if (v1 & 1ULL) {
            LS_PAUSE();
            continue;
        }
        out = published_;
        v2  = version_.load(std::memory_order_acquire);
    } while (v1 != v2);
    return out;
}

}  // namespace line_scanner
