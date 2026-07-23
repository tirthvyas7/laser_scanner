#pragma once

#include <cstddef>

namespace line_scanner {

// The element type a block emits. Blocks declare theirs via outputType(); the
// orchestrator maps it to a concrete RingBuffer<T> when wiring. Sink => None.
enum class ChannelType { None, PixelPair, LabelledElement, DefectRecord };

// Non-templated interface every channel exposes, so the orchestrator and
// blocks can hold heterogeneous-element channels behind one type
// (shared_ptr<ChannelBase>) and drive their lifecycle/metrics generically.
//
// This carries ONLY the off-hot-path operations. The per-pixel read()/write()
// stay NON-virtual on RingBuffer<T> — a consumer downcasts its ChannelBase to
// the concrete RingBuffer<T> once at wiring and uses those directly, so there
// are zero vtable lookups on the hot path. The vtable added here sits with the
// cold cap_/mask_/buf_ members and never shares a cache line with the hot
// w_/r_ atomics, so the L1 / false-sharing discipline is unchanged.
struct ChannelBase {
    virtual ~ChannelBase() = default;

    virtual void                     close() noexcept        = 0;
    [[nodiscard]] virtual bool       isClosed() const noexcept   = 0;
    [[nodiscard]] virtual bool       isEmpty() const noexcept    = 0;
    [[nodiscard]] virtual size_t     occupancy() const noexcept  = 0;
    [[nodiscard]] virtual size_t     capacity() const noexcept   = 0;
    [[nodiscard]] virtual size_t     memoryBytes() const noexcept = 0;
};

}  // namespace line_scanner
