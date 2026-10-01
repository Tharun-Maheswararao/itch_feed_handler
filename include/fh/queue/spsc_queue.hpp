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
//  * Optional batching (set_batch(k)): each side publishes its index only
//    every k messages, cutting cross-core cache-line transfers by ~k. A side
//    always publishes before it would wait (consumer finds the ring empty,
//    producer finds it full), and the producer calls flush() whenever it goes
//    idle, so batching never stalls progress. k = 1 publishes after every
//    message, the classic behaviour.
#pragma once

#include <algorithm>
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

    // Publish each index every `k` messages (1 = every message). Set before
    // the threads start; clamped to [1, Capacity / 2].
    void set_batch(std::size_t k) noexcept { batch_ = std::clamp<std::size_t>(k, 1, Capacity / 2); }
    std::size_t batch() const noexcept { return batch_; }

    // Producer only.
    bool try_push(const T& v) noexcept {
        Producer& p = prod_.value;
        if (p.head - p.tail_cache == Capacity) {
            publish_head();  // the consumer may be waiting for what we hold back
            p.tail_cache = tail_.value.load(std::memory_order_acquire);
            if (p.head - p.tail_cache == Capacity) return false;
        }
        slots_[p.head & kMask] = v;
        ++p.head;
        if (p.head - p.published >= batch_) publish_head();
        return true;
    }

    void push(const T& v) noexcept {
        while (!try_push(v)) cpu_relax();
    }

    // Producer only: make every pushed message visible now.
    void flush() noexcept { publish_head(); }

    // Consumer only.
    bool try_pop(T& out) noexcept {
        Consumer& c = cons_.value;
        if (c.tail == c.head_cache) {
            publish_tail();  // hand the space back before we wait
            c.head_cache = head_.value.load(std::memory_order_acquire);
            if (c.tail == c.head_cache) return false;
        }
        out = slots_[c.tail & kMask];
        ++c.tail;
        if (c.tail - c.published >= batch_) publish_tail();
        return true;
    }

    void pop(T& out) noexcept {
        while (!try_pop(out)) cpu_relax();
    }

    // Consumer only: messages published but not yet popped (one acquire load
    // of the producer's index, so call it sparingly).
    std::size_t consumer_depth() const noexcept {
        return static_cast<std::size_t>(head_.value.load(std::memory_order_acquire) - cons_.value.tail);
    }

    // Approximate (published indices only); safe to call from either side.
    std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(head_.value.load(std::memory_order_acquire) -
                                        tail_.value.load(std::memory_order_acquire));
    }

private:
    template <class V>
    struct alignas(kCacheLine) Padded {
        V value{};
    };
    struct Producer {          // producer-private line
        uint64_t head = 0;       // next slot to write
        uint64_t published = 0;  // last value stored to head_
        uint64_t tail_cache = 0; // producer's view of tail_
    };
    struct Consumer {          // consumer-private line
        uint64_t tail = 0;
        uint64_t published = 0;
        uint64_t head_cache = 0;
    };

    void publish_head() noexcept {
        Producer& p = prod_.value;
        if (p.published != p.head) {
            head_.value.store(p.head, std::memory_order_release);
            p.published = p.head;
        }
    }
    void publish_tail() noexcept {
        Consumer& c = cons_.value;
        if (c.published != c.tail) {
            tail_.value.store(c.tail, std::memory_order_release);
            c.published = c.tail;
        }
    }

    Padded<std::atomic<uint64_t>> head_;  // shared: written by producer
    Padded<Producer> prod_;
    Padded<std::atomic<uint64_t>> tail_;  // shared: written by consumer
    Padded<Consumer> cons_;
    T* const slots_;
    std::size_t batch_ = 1;  // read-only once the threads run
    char pad_[kCacheLine - sizeof(T*) - sizeof(std::size_t)];
};

}  // namespace fh
