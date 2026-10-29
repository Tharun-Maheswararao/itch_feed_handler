// Fixed-capacity object pool with a free list. All memory is reserved at
// construction; acquire() and release() never allocate.
//
// Handles are 32-bit indices. Handle 0 is reserved as "null", so a
// default-initialised handle field means "none" without an extra flag.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace fh {

template <class T>
class Pool {
public:
    explicit Pool(uint32_t capacity) : nodes_(static_cast<std::size_t>(capacity) + 1), free_(capacity) {
        if (capacity == 0 || capacity == UINT32_MAX) throw std::invalid_argument("pool capacity");
        // Free list holds capacity..1, so acquire() hands out 1, 2, 3, ...
        for (uint32_t i = 0; i < capacity; ++i) free_[i] = capacity - i;
        top_ = capacity;
    }

    // Returns 0 when the pool is exhausted (the caller counts it).
    uint32_t acquire() noexcept {
        if (top_ == 0) return 0;
        const uint32_t h = free_[--top_];
        nodes_[h] = T{};
        if (in_use() > peak_) peak_ = in_use();
        return h;
    }

    void release(uint32_t h) noexcept { free_[top_++] = h; }

    T& operator[](uint32_t h) noexcept { return nodes_[h]; }
    const T& operator[](uint32_t h) const noexcept { return nodes_[h]; }

    uint32_t capacity() const noexcept { return static_cast<uint32_t>(free_.size()); }
    uint32_t in_use() const noexcept { return capacity() - top_; }
    uint32_t peak() const noexcept { return peak_; }

private:
    std::vector<T> nodes_;
    std::vector<uint32_t> free_;
    uint32_t top_ = 0;
    uint32_t peak_ = 0;
};

}  // namespace fh
