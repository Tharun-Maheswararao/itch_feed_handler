// Serialized timer pair for per-message latency (PerfGuard).
//
// now_ticks() in clock.hpp is a bare RDTSC: cheap, fine for sampled
// pipeline stamps, but the CPU may execute it before earlier instructions
// finish or let later ones start before it. Timing one book update of a few
// dozen cycles needs the read pinned in program order:
//
//   start: lfence; rdtsc; lfence   earlier work retired, later work not begun
//   stop:  rdtscp; lfence          rdtscp waits for the measured work; the
//                                  lfence keeps later work from starting early
//
// The pair itself costs tens of cycles. calibrate_timer_overhead() measures an
// empty pair and the harness subtracts that from every sample, otherwise p50
// is mostly timer cost.
//
// AArch64: ISB before and after the counter read. Apple M-series counters
// advance in 41.67 ns steps, which is coarser than a book update, so arm64
// numbers are reported but never used as a timing gate.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "fh/stats/clock.hpp"

namespace fh {

inline uint64_t timer_start() noexcept {
#if defined(__x86_64__)
    uint32_t lo, hi;
    asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi)::"memory");
    return (uint64_t{hi} << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(v)::"memory");
    return v;
#else
    return now_ticks();
#endif
}

inline uint64_t timer_stop() noexcept {
#if defined(__x86_64__)
    uint32_t lo, hi;
    asm volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi)::"rcx", "memory");
    return (uint64_t{hi} << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(v)::"memory");
    return v;
#else
    return now_ticks();
#endif
}

inline const char* timer_name() noexcept {
#if defined(__x86_64__)
    return "lfence+rdtsc / rdtscp+lfence";
#elif defined(__aarch64__)
    return "isb+cntvct_el0";
#else
    return "steady_clock";
#endif
}

struct TimerOverhead {
    uint64_t min_ticks = 0;
    uint64_t median_ticks = 0;  // what the harness subtracts
    uint64_t p99_ticks = 0;
};

// Cost of an empty start/stop pair. Runs before the replay, so the vector
// allocation here is outside any armed allocation window.
inline TimerOverhead calibrate_timer_overhead(std::size_t pairs = 1'000'000) {
    std::vector<uint64_t> d(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        const uint64_t a = timer_start();
        const uint64_t b = timer_stop();
        d[i] = b - a;
    }
    std::sort(d.begin(), d.end());
    return {d.front(), d[pairs / 2], d[pairs * 99 / 100]};
}

}  // namespace fh
