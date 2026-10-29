// Global operator new/delete replacements that count allocations.
// See include/fh/stats/alloc_counter.hpp. Linked only into tests and tools.
#include "fh/stats/alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<uint64_t> g_allocs{0};
std::atomic<uint64_t> g_bytes{0};

void* counted_alloc(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* counted_aligned(std::size_t n, std::align_val_t al) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
    void* p = nullptr;
    std::size_t a = static_cast<std::size_t>(al);
    if (a < sizeof(void*)) a = sizeof(void*);
    return posix_memalign(&p, a, n ? n : 1) == 0 ? p : nullptr;
}
}  // namespace

namespace fh {
uint64_t allocation_count() noexcept { return g_allocs.load(std::memory_order_relaxed); }
uint64_t allocated_bytes() noexcept { return g_bytes.load(std::memory_order_relaxed); }
}  // namespace fh

// --- throwing forms -------------------------------------------------------
void* operator new(std::size_t n) {
    if (void* p = counted_alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = counted_alloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t al) {
    if (void* p = counted_aligned(n, al)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t al) {
    if (void* p = counted_aligned(n, al)) return p;
    throw std::bad_alloc();
}

// --- nothrow forms ----------------------------------------------------------
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return counted_aligned(n, al); }
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return counted_aligned(n, al);
}

// --- every delete form ------------------------------------------------------
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
