#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#define LS_PAUSE() _mm_pause()
#elif defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#define LS_PAUSE() _mm_pause()
#else
#define LS_PAUSE() ((void)0)
#endif

namespace line_scanner {

#ifdef __cpp_lib_hardware_interference_size
constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
constexpr size_t kCacheLineSize = 64;
#endif

template <typename T>
class RingBuffer {
   public:
    explicit RingBuffer(size_t requested_capacity)
        : cap_(roundUpPow2(requested_capacity == 0 ? 1 : requested_capacity)),
          mask_(cap_ - 1),
          buf_(cap_) {}

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    [[nodiscard]] bool write(const T& val) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        if (w - r >= cap_) {
            return false;
        }
        buf_[w & mask_] = val;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool read(T& out) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        if (r == w) {
            return false;
        }
        out = buf_[r & mask_];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

    void close() noexcept { closed_.store(true, std::memory_order_release); }

    [[nodiscard]] bool isClosed() const noexcept { return closed_.load(std::memory_order_acquire); }

    [[nodiscard]] bool isEmpty() const noexcept {
        return r_.load(std::memory_order_acquire) == w_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t capacity() const noexcept { return cap_; }

    [[nodiscard]] size_t occupancy() const noexcept {
        const size_t w = w_.load(std::memory_order_acquire);
        const size_t r = r_.load(std::memory_order_acquire);
        return w - r;
    }

    [[nodiscard]] size_t memoryBytes() const noexcept { return cap_ * sizeof(T); }

   private:
    static size_t roundUpPow2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const size_t   cap_;
    const size_t   mask_;
    std::vector<T> buf_;

    alignas(kCacheLineSize) std::atomic<size_t> w_{0};
    alignas(kCacheLineSize) std::atomic<size_t> r_{0};
    alignas(kCacheLineSize) std::atomic<bool> closed_{false};
};

}  // namespace line_scanner
