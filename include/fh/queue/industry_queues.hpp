// Thin adapters giving two widely used SPSC queues the SpscQueue interface:
//
//   RigtorpQueue  rigtorp::SPSCQueue  (vendored, third_party/rigtorp/VERSION)
//   BoostQueue    boost::lockfree::spsc_queue with compile-time capacity
//                 (only when Boost is found: FH_HAVE_BOOST)
//
// Only each library's public API is used. Blocking push/pop spin with the
// same cpu_relax() loop as SpscQueue, so all lock-free queues wait the same
// way and the comparison measures the queues, not the wait policy.
#pragma once

#include <cstddef>
#include <memory>

#include "fh/util/platform.hpp"
#include "rigtorp/SPSCQueue.h"

#if defined(FH_HAVE_BOOST)
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/version.hpp>
#endif

namespace fh {

template <class T, std::size_t Capacity>
class RigtorpQueue {
public:
    RigtorpQueue() : q_(Capacity) {}
    static constexpr std::size_t capacity() { return Capacity; }
    void set_batch(std::size_t) noexcept {}
    std::size_t batch() const noexcept { return 1; }
    void flush() noexcept {}

    bool try_push(const T& v) noexcept { return q_.try_push(v); }
    void push(const T& v) noexcept {
        while (!q_.try_push(v)) cpu_relax();
    }
    bool try_pop(T& out) noexcept {
        T* p = q_.front();
        if (!p) return false;
        out = *p;
        q_.pop();
        return true;
    }
    void pop(T& out) noexcept {
        while (!try_pop(out)) cpu_relax();
    }
    std::size_t consumer_depth() const noexcept { return q_.size(); }

private:
    rigtorp::SPSCQueue<T> q_;
};

inline const char* rigtorp_version() { return "rigtorp/SPSCQueue@1053918"; }

#if defined(FH_HAVE_BOOST)
template <class T, std::size_t Capacity>
class BoostQueue {
public:
    static constexpr std::size_t capacity() { return Capacity; }
    void set_batch(std::size_t) noexcept {}
    std::size_t batch() const noexcept { return 1; }
    void flush() noexcept {}

    bool try_push(const T& v) noexcept { return q_.push(v); }
    void push(const T& v) noexcept {
        while (!q_.push(v)) cpu_relax();
    }
    bool try_pop(T& out) noexcept { return q_.pop(out); }
    void pop(T& out) noexcept {
        while (!q_.pop(out)) cpu_relax();
    }
    std::size_t consumer_depth() const noexcept { return q_.read_available(); }

private:
    boost::lockfree::spsc_queue<T, boost::lockfree::capacity<Capacity>> q_;
};

inline const char* boost_version() { return "boost " BOOST_LIB_VERSION; }
#endif

}  // namespace fh
