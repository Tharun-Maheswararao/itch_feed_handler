// Baseline: a bounded queue guarded by one std::mutex, with condition
// variables for "not empty" and "not full". Same interface as SpscQueue, so
// the pipeline swaps between them with one template argument.
//
// This is the conventional way to hand data between threads: correct, simple,
// and every push and pop takes the lock. A blocked side sleeps in the kernel
// and must be woken, which is what dominates its latency.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>

namespace fh {

template <class T, std::size_t Capacity>
class MutexQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr std::size_t kMask = Capacity - 1;

public:
    MutexQueue() : slots_(std::make_unique<T[]>(Capacity)) {}
    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    static constexpr std::size_t capacity() { return Capacity; }
    void set_batch(std::size_t) noexcept {}  // not applicable
    std::size_t batch() const noexcept { return 1; }
    void flush() noexcept {}                 // every push is visible at unlock

    bool try_push(const T& v) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (head_ - tail_ == Capacity) return false;
            slots_[head_ & kMask] = v;
            ++head_;
        }
        not_empty_.notify_one();
        return true;
    }

    void push(const T& v) {
        {
            std::unique_lock<std::mutex> lk(m_);
            not_full_.wait(lk, [&] { return head_ - tail_ < Capacity; });
            slots_[head_ & kMask] = v;
            ++head_;
        }
        not_empty_.notify_one();
    }

    bool try_pop(T& out) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (head_ == tail_) return false;
            out = slots_[tail_ & kMask];
            ++tail_;
        }
        not_full_.notify_one();
        return true;
    }

    void pop(T& out) {
        {
            std::unique_lock<std::mutex> lk(m_);
            not_empty_.wait(lk, [&] { return head_ != tail_; });
            out = slots_[tail_ & kMask];
            ++tail_;
        }
        not_full_.notify_one();
    }

    std::size_t consumer_depth() const {
        std::lock_guard<std::mutex> lk(m_);
        return static_cast<std::size_t>(head_ - tail_);
    }

private:
    mutable std::mutex m_;
    std::condition_variable not_empty_, not_full_;
    std::unique_ptr<T[]> slots_;
    uint64_t head_ = 0, tail_ = 0;  // guarded by m_
};

}  // namespace fh
