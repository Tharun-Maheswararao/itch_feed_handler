// Lock-free single-producer / single-consumer ring buffer.
//
//  * Power-of-two capacity: index wrap is `i & kMask`, not a modulo.
//  * head_ is written only by the producer, tail_ only by the consumer.
//    Indices grow monotonically (64-bit, never wrap in practice) so
//    full/empty is just `head - tail == Capacity` / `head == tail`.
//  * Release store on publish, acquire load on observe: the consumer that sees
//    the new head also sees the slot contents written before it.
//  * Each index, and each side's cached copy of the other's index, lives on its
//    own cache line, so the two cores never false-share.
//  * The cached copy means a side reads the other core's line only when it
//    *thinks* the queue is full (producer) or empty (consumer).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "fh/util/platform.hpp"

namespace fh {

template <class T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscQueue() : slots_(static_cast<T*>(::operator new[](Capacity * sizeof(T), std::align_val_t{kCacheLine}))) {}
    ~SpscQueue() { ::operator delete[](slots_, std::align_val_t{kCacheLine}); }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    static constexpr std::size_t capacity() { return Capacity; }

    // Producer only.
    bool try_push(const T& v) noexcept {
        const uint64_t h = head_.value.load(std::memory_order_relaxed);
        if (h - tail_cache_.value == Capacity) {
            tail_cache_.value = tail_.value.load(std::memory_order_acquire);
            if (h - tail_cache_.value == Capacity) return false;
        }
        slots_[h & kMask] = v;
        head_.value.store(h + 1, std::memory_order_release);
        return true;
    }

    void push(const T& v) noexcept {
        while (!try_push(v)) cpu_relax();
    }

    // Consumer only.
    bool try_pop(T& out) noexcept {
        const uint64_t t = tail_.value.load(std::memory_order_relaxed);
        if (t == head_cache_.value) {
            head_cache_.value = head_.value.load(std::memory_order_acquire);
            if (t == head_cache_.value) return false;
        }
        out = slots_[t & kMask];
        tail_.value.store(t + 1, std::memory_order_release);
        return true;
    }

    void pop(T& out) noexcept {
        while (!try_pop(out)) cpu_relax();
    }

    // Approximate; safe to call from either side.
    std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(head_.value.load(std::memory_order_acquire) -
                                        tail_.value.load(std::memory_order_acquire));
    }

private:
    template <class V>
    struct alignas(kCacheLine) Padded {
        V value{};
    };

    Padded<std::atomic<uint64_t>> head_;  // written by producer
    Padded<uint64_t> tail_cache_;         // producer's view of tail_
    Padded<std::atomic<uint64_t>> tail_;  // written by consumer
    Padded<uint64_t> head_cache_;         // consumer's view of head_
    T* const slots_;
    char pad_[kCacheLine - sizeof(T*)];  // keep neighbours off the last line
};

}  // namespace fh
