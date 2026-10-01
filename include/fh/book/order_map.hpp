// Preallocated open-addressing hash map from order reference to order.
//
//  * Linear probing over a power-of-two table: one cache line usually holds
//    the home slot and the next few probes.
//  * Fibonacci (multiplicative) hashing; ITCH references are roughly
//    sequential, which a plain mask would cluster badly.
//  * Backward-shift deletion instead of tombstones, so a day of adds and
//    deletes never degrades probe lengths.
//  * Key 0 marks an empty slot. A real reference of 0 is held out-of-line.
//  * Grows (rehash) only if the load factor passes 1/2, which a correctly
//    sized table never does; the grow count is reported so it is visible.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "fh/parser/message.hpp"

namespace fh {

struct OrderEntry {
    uint64_t ref;  // 0 = empty
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    Side side;
};
static_assert(sizeof(OrderEntry) == 24);

class OrderMap {
public:
    explicit OrderMap(std::size_t capacity_pow2 = std::size_t{1} << 23) { allocate(capacity_pow2); }
    ~OrderMap() { release(); }
    OrderMap(const OrderMap&) = delete;
    OrderMap& operator=(const OrderMap&) = delete;

    // Returns the entry for `ref`, or nullptr.
    OrderEntry* find(uint64_t ref) noexcept {
        if (ref == 0) [[unlikely]]
            return zero_used_ ? &zero_ : nullptr;
        for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
            OrderEntry& e = slots_[i];
            if (e.ref == ref) return &e;
            if (e.ref == 0) return nullptr;
        }
    }

    // Inserts and returns the new entry, or the existing one with
    // `inserted=false` if the reference is already present.
    OrderEntry* insert(uint64_t ref, bool& inserted) {
        if (ref == 0) [[unlikely]] {
            inserted = !zero_used_;
            zero_used_ = true;
            zero_.ref = 0;
            return &zero_;
        }
        if ((size_ + 1) * 2 > capacity_) [[unlikely]]
            grow();
        for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
            OrderEntry& e = slots_[i];
            if (e.ref == ref) {
                inserted = false;
                return &e;
            }
            if (e.ref == 0) {
                e.ref = ref;
                ++size_;
                inserted = true;
                return &e;
            }
        }
    }

    // Erase the entry previously returned by find()/insert().
    void erase(OrderEntry* e) noexcept {
        if (e == &zero_) [[unlikely]] {
            zero_used_ = false;
            return;
        }
        std::size_t hole = static_cast<std::size_t>(e - slots_);
        // Backward shift: pull later entries of the probe chain into the hole
        // whenever the hole lies cyclically between their home and their slot.
        for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
            OrderEntry& cand = slots_[j];
            if (cand.ref == 0) break;
            const std::size_t h = home(cand.ref);
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = cand;
                hole = j;
            }
        }
        slots_[hole].ref = 0;
        --size_;
    }

    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < capacity_; ++i)
            if (slots_[i].ref != 0) f(slots_[i]);
        if (zero_used_) f(zero_);
    }

    std::size_t size() const noexcept { return size_ + (zero_used_ ? 1 : 0); }
    std::size_t capacity() const noexcept { return capacity_; }
    uint64_t grows() const noexcept { return grows_; }

private:
    std::size_t home(uint64_t ref) const noexcept {
        return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void allocate(std::size_t cap) {
        if (cap < 16 || (cap & (cap - 1)) != 0) throw std::invalid_argument("OrderMap capacity must be a power of two >= 16");
        capacity_ = cap;
        mask_ = cap - 1;
        shift_ = 64 - __builtin_ctzll(cap);
        bytes_ = cap * sizeof(OrderEntry);
        constexpr std::size_t kHuge = std::size_t{2} << 20;
        bytes_ = (bytes_ + kHuge - 1) / kHuge * kHuge;
        void* p = std::aligned_alloc(kHuge, bytes_);
        if (!p) throw std::bad_alloc();
#if defined(__linux__)
        ::madvise(p, bytes_, MADV_HUGEPAGE);  // fewer TLB misses on random probes
#endif
        std::memset(p, 0, bytes_);  // also pre-faults every page
        slots_ = static_cast<OrderEntry*>(p);
        size_ = 0;
    }

    void release() noexcept { std::free(slots_); }

    void grow() {
        OrderEntry* old = slots_;
        const std::size_t old_cap = capacity_;
        allocate(old_cap * 2);
        for (std::size_t i = 0; i < old_cap; ++i) {
            if (old[i].ref == 0) continue;
            std::size_t j = home(old[i].ref);
            while (slots_[j].ref != 0) j = (j + 1) & mask_;
            slots_[j] = old[i];
            ++size_;
        }
        std::free(old);
        ++grows_;
    }

    OrderEntry* slots_ = nullptr;
    std::size_t capacity_ = 0, mask_ = 0, size_ = 0, bytes_ = 0;
    int shift_ = 0;
    uint64_t grows_ = 0;
    OrderEntry zero_{};
    bool zero_used_ = false;
};

}  // namespace fh
