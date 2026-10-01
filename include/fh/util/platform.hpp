// Small platform shims: cache line size, spin-wait hint, thread pinning.
#pragma once

#include <cstddef>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace fh {

// Apple M-series cores use 128-byte cache lines; x86 uses 64, and its
// adjacent-line prefetcher makes 128 a safer pad there too. We pad to the
// real line size per target and document the choice.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// Pin the calling thread to one CPU. Returns false if the OS cannot do it
// (macOS has no affinity API; there we raise QoS so the scheduler keeps the
// thread on a performance core instead).
inline bool pin_current_thread(int cpu) noexcept {
#if defined(__linux__)
    if (cpu < 0) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#elif defined(__APPLE__)
    (void)cpu;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return false;
#else
    (void)cpu;
    return false;
#endif
}

}  // namespace fh
