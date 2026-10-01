// Cheap timestamp source for latency stamps.
//
//   x86-64  : RDTSC (invariant TSC on any modern server), calibrated against
//             steady_clock at startup.
//   AArch64 : CNTVCT_EL0 virtual counter, frequency read from CNTFRQ_EL0.
//             Units and resolution can differ: Apple M4 reports a 1 GHz
//             counter that advances in 41.67 ns steps (24 MHz underneath).
//             clock_resolution_ns() measures the real step, and it is
//             reported with every result.
//   other   : std::chrono::steady_clock (also forced by -DFH_USE_CHRONO=ON).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if !defined(FH_USE_CHRONO) && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

namespace fh {

inline uint64_t now_ticks() noexcept {
#if defined(FH_USE_CHRONO)
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#elif defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    // ISB stops the counter read from being hoisted above earlier work.
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline const char* clock_source_name() noexcept {
#if defined(FH_USE_CHRONO)
    return "steady_clock";
#elif defined(__x86_64__) || defined(__i386__)
    return "rdtsc";
#elif defined(__aarch64__)
    return "cntvct_el0";
#else
    return "steady_clock";
#endif
}

// Ticks per second of now_ticks().
inline double ticks_per_second() {
    static const double hz = [] {
#if !defined(FH_USE_CHRONO) && defined(__aarch64__)
        uint64_t f;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
        return static_cast<double>(f);
#elif !defined(FH_USE_CHRONO) && (defined(__x86_64__) || defined(__i386__))
        using clk = std::chrono::steady_clock;
        const auto c0 = clk::now();
        const uint64_t t0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto c1 = clk::now();
        const uint64_t t1 = __rdtsc();
        const double secs = std::chrono::duration<double>(c1 - c0).count();
        return static_cast<double>(t1 - t0) / secs;
#else
        using period = std::chrono::steady_clock::period;
        return static_cast<double>(period::den) / static_cast<double>(period::num);
#endif
    }();
    return hz;
}

// Typical (median) non-zero step between two consecutive reads, in ns. This
// is the real resolution: the units of the counter can be finer than its
// update rate.
inline double clock_resolution_ns() {
    static const double res = [] {
        std::vector<uint64_t> steps;
        steps.reserve(1 << 16);
        uint64_t a = now_ticks();
        for (int i = 0; i < 4'000'000 && steps.size() < (1u << 16); ++i) {
            const uint64_t b = now_ticks();
            if (b != a) steps.push_back(b - a);
            a = b;
        }
        if (steps.empty()) return 0.0;
        std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2), steps.end());
        return static_cast<double>(steps[steps.size() / 2]) * 1e9 / ticks_per_second();
    }();
    return res;
}

inline double ticks_to_ns(double ticks) { return ticks * 1e9 / ticks_per_second(); }
inline uint64_t ns_to_ticks(double ns) { return static_cast<uint64_t>(ns * ticks_per_second() / 1e9); }

}  // namespace fh
