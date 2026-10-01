// Single-writer seqlock for small trivially copyable snapshots.
//
// The writer never waits: it bumps the sequence to odd, writes, bumps it to
// even. A reader copies the data and retries if the sequence was odd or
// changed during the copy. The payload is stored as relaxed atomic words so
// the concurrent read is well defined in the C++ memory model (no data race),
// following Boehm, "Can seqlocks get along with programming language memory
// models?" (2012).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "fh/util/platform.hpp"

namespace fh {

template <class T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr std::size_t kWords = (sizeof(T) + 7) / 8;

public:
    // Writer only. Never blocks.
    void store(const T& v) noexcept {
        std::array<uint64_t, kWords> buf{};
        std::memcpy(buf.data(), &v, sizeof(T));
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i) words_[i].store(buf[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    // Reader. Returns false if no snapshot has been written yet.
    bool load(T& out) const noexcept {
        std::array<uint64_t, kWords> buf;
        uint64_t s1, s2;
        do {
            s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) {
                cpu_relax();
                continue;
            }
            for (std::size_t i = 0; i < kWords; ++i) buf[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = seq_.load(std::memory_order_relaxed);
            if (s1 == s2) break;
            retries_.fetch_add(1, std::memory_order_relaxed);
        } while (true);
        if (s1 == 0) return false;
        std::memcpy(&out, buf.data(), sizeof(T));
        return true;
    }

    uint64_t version() const noexcept { return seq_.load(std::memory_order_acquire); }
    uint64_t retries() const noexcept { return retries_.load(std::memory_order_relaxed); }

private:
    alignas(kCacheLine) std::atomic<uint64_t> seq_{0};
    std::array<std::atomic<uint64_t>, kWords> words_{};
    mutable std::atomic<uint64_t> retries_{0};
};

}  // namespace fh
